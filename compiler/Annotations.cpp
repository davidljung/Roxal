#include "Annotations.h"

#include "Object.h"
#include "VM.h"

#include <core/AST.h>

#include <stdexcept>

namespace roxal {

namespace {

// ---- static conversion ----------------------------------------------------

AnnotationArg staticArg(const ptr<ast::Expression>& e);

AnnotationArg numArg(const std::variant<int32_t, int64_t, double>& n)
{
    AnnotationArg a;
    if (std::holds_alternative<int32_t>(n)) {
        a.kind = AnnotationArg::Kind::Int;
        a.integer = int64_t(std::get<int32_t>(n));
    } else if (std::holds_alternative<int64_t>(n)) {
        a.kind = AnnotationArg::Kind::Int;
        a.integer = std::get<int64_t>(n);
    } else {
        a.kind = AnnotationArg::Kind::Real;
        a.real = std::get<double>(n);
    }
    return a;
}

// Fold a unary minus into the literal it applies to, so clients never see a
// negation node.  A suffixed literal negates the literal under the suffix.
AnnotationArg negateArg(AnnotationArg a)
{
    switch (a.kind) {
        case AnnotationArg::Kind::Int:
            a.integer = -a.integer;
            return a;
        case AnnotationArg::Kind::Real:
            a.real = -a.real;
            return a;
        case AnnotationArg::Kind::Suffixed:
            a.items.at(0) = negateArg(a.items.at(0));
            return a;
        default:
            // The compiler admits Negate of any admitted argument, so '-"x"'
            // reaches here; the evaluating path raises on it too.
            throw std::runtime_error("annotation argument: '-' applied to a non-number");
    }
}

AnnotationArg suffixedArg(AnnotationArg literal, const ustring& suffix)
{
    AnnotationArg a;
    a.kind = AnnotationArg::Kind::Suffixed;
    a.suffix = suffix;
    a.items.push_back(std::move(literal));
    return a;
}

AnnotationArg staticArg(const ptr<ast::Expression>& e)
{
    using namespace ast;
    AnnotationArg a;
    if (!e)
        return a;                      // Nil
    if (auto s = dynamic_ptr_cast<Str>(e)) {
        a.kind = AnnotationArg::Kind::String;
        a.text = s->str;
        return a;
    }
    if (auto n = dynamic_ptr_cast<Num>(e))
        return numArg(n->num);
    if (auto b = dynamic_ptr_cast<Bool>(e)) {
        a.kind = AnnotationArg::Kind::Bool;
        a.boolean = b->value;
        return a;
    }
    if (auto sn = dynamic_ptr_cast<SuffixedNum>(e))
        return suffixedArg(numArg(sn->num), sn->suffix);
    if (auto ss = dynamic_ptr_cast<SuffixedStr>(e)) {
        AnnotationArg lit;
        lit.kind = AnnotationArg::Kind::String;
        lit.text = ss->str;
        return suffixedArg(std::move(lit), ss->suffix);
    }
    if (auto l = dynamic_ptr_cast<List>(e)) {
        a.kind = AnnotationArg::Kind::List;
        a.items.reserve(l->elements.size());
        for (const auto& el : l->elements)
            a.items.push_back(staticArg(el));
        return a;
    }
    if (auto d = dynamic_ptr_cast<Dict>(e)) {
        a.kind = AnnotationArg::Kind::Dict;
        a.entries.reserve(d->entries.size());
        for (const auto& entry : d->entries)
            a.entries.emplace_back(staticArg(entry.first), staticArg(entry.second));
        return a;
    }
    if (auto u = dynamic_ptr_cast<UnaryOp>(e)) {
        if (u->op != UnaryOp::Negate)
            throw std::runtime_error("annotation argument is not a literal");
        return negateArg(staticArg(u->arg));
    }
    if (auto v = dynamic_ptr_cast<Variable>(e)) {
        a.kind = AnnotationArg::Kind::Name;
        a.text = v->name;
        return a;
    }
    // `nil` is a bare Literal rather than a node type of its own, so it has to
    // be recognised after the specific literal kinds above (which derive from it)
    if (auto lit = dynamic_ptr_cast<Literal>(e))
        if (lit->literalType == Literal::LiteralType::Nil)
            return a;                  // Nil
    // Anything else is rejected at compile time (see
    // RoxalCompiler::checkAnnotationArgs), so reaching here means a node kind
    // was added without teaching this converter about it.
    throw std::runtime_error("annotation argument is not a literal");
}


// ---- evaluation (the `inspect` path) --------------------------------------

Value numValue(const std::variant<int32_t, int64_t, double>& n)
{
    if (std::holds_alternative<int32_t>(n))
        return Value::intVal(int64_t(std::get<int32_t>(n)));
    if (std::holds_alternative<int64_t>(n))
        return Value::intVal(std::get<int64_t>(n));
    return Value::realVal(std::get<double>(n));
}

// Resolve a bare name in an annotation argument: the declaring module's
// variables first, then the globals (which is where the sys surface lives).
Value resolveName(const ustring& name, ObjModuleType* owner, VM& vm)
{
    if (owner) {
        auto v = owner->vars.load(name.hashCode());
        if (v.has_value())
            return v.value();
    }
    auto g = vm.loadGlobal(name);
    if (g.has_value())
        return g.value();
    return Value::nilVal();
}

// Apply a literal suffix (2s, 100ms, 5kg) by calling the @suffix-registered
// function, exactly as a compiled suffixed literal would.  Registrations live
// on the module that declared them; sys's are visible everywhere.
Value applySuffix(const ustring& suffix, const Value& literal, ObjModuleType* owner, VM& vm)
{
    ustring funcName;
    ObjModuleType* declaring = nullptr;
    if (owner) {
        auto it = owner->registeredSuffixes.find(suffix);
        if (it != owner->registeredSuffixes.end()) {
            funcName = it->second;
            declaring = owner;
        }
    }
    if (funcName.isEmpty()) {
        Value sysMod = vm.getBuiltinModuleType(toUnicodeString("sys"));
        if (sysMod.isObj() && isModuleType(sysMod)) {
            ObjModuleType* sys = asModuleType(sysMod);
            auto it = sys->registeredSuffixes.find(suffix);
            if (it != sys->registeredSuffixes.end()) {
                funcName = it->second;
                declaring = sys;
            }
        }
    }
    if (funcName.isEmpty() || !declaring)
        throw std::runtime_error("annotation uses unknown literal suffix '"
                                 + toUTF8StdString(suffix) + "'");

    auto fv = declaring->vars.load(funcName.hashCode());
    if (!fv.has_value() || !isClosure(fv.value()))
        throw std::runtime_error("suffix function '" + toUTF8StdString(funcName)
                                 + "' is not callable");
    auto result = vm.invokeClosure(asClosure(fv.value()), { literal });
    if (result.first != ExecutionStatus::OK)
        throw std::runtime_error("evaluating literal suffix '"
                                 + toUTF8StdString(suffix) + "' failed");
    return result.second;
}

// The recursion below: the cover is checked once at the public entry point, so
// the internal calls do not thread the token.
Value evalArg(const ptr<ast::Expression>& e, ObjModuleType* owner, VM& vm)
{
    using namespace ast;
    if (!e)
        return Value::nilVal();
    if (auto s = dynamic_ptr_cast<Str>(e))
        return Value::stringVal(s->str);
    if (auto n = dynamic_ptr_cast<Num>(e))
        return numValue(n->num);
    if (auto b = dynamic_ptr_cast<Bool>(e))
        return b->value ? Value::trueVal() : Value::falseVal();
    if (auto sn = dynamic_ptr_cast<SuffixedNum>(e))
        return applySuffix(sn->suffix, numValue(sn->num), owner, vm);
    if (auto ss = dynamic_ptr_cast<SuffixedStr>(e))
        return applySuffix(ss->suffix, Value::stringVal(ss->str), owner, vm);
    if (auto l = dynamic_ptr_cast<List>(e)) {
        Value lst = Value::listVal();
        for (const auto& el : l->elements)
            asList(lst)->append(evalArg(el, owner, vm));
        return lst;
    }
    if (auto d = dynamic_ptr_cast<Dict>(e)) {
        Value dict = Value::dictVal();
        for (const auto& entry : d->entries)
            asDict(dict)->store(evalArg(entry.first, owner, vm),
                                evalArg(entry.second, owner, vm));
        return dict;
    }
    if (auto u = dynamic_ptr_cast<UnaryOp>(e)) {
        if (u->op != UnaryOp::Negate)
            throw std::runtime_error("annotation argument is not a literal");
        return roxal::negate(evalArg(u->arg, owner, vm));
    }
    if (auto v = dynamic_ptr_cast<Variable>(e))
        return resolveName(v->name, owner, vm);
    // see the matching comment in staticArg()
    if (auto lit = dynamic_ptr_cast<Literal>(e))
        if (lit->literalType == Literal::LiteralType::Nil)
            return Value::nilVal();
    throw std::runtime_error("annotation argument is not a literal");
}

bool listHasAnnotation(const std::vector<ptr<ast::Annotation>>& annotations, const char* name)
{
    ustring wanted = toUnicodeString(name);
    for (const auto& a : annotations)
        if (a && a->name == wanted)
            return true;
    return false;
}

} // anonymous namespace


const AnnotationArg* AnnotationView::arg(const ustring& argName) const
{
    for (const auto& entry : named)
        if (entry.first == argName)
            return &entry.second;
    return nullptr;
}


AnnotationView annotationView(const ast::Annotation& annot)
{
    AnnotationView view;
    view.name = annot.name;
    for (const auto& arg : annot.args) {
        if (arg.first.isEmpty())
            view.args.push_back(staticArg(arg.second));
        else
            view.named.emplace_back(arg.first, staticArg(arg.second));
    }
    return view;
}

std::vector<AnnotationView> annotationViews(const std::vector<ptr<ast::Annotation>>& annotations)
{
    std::vector<AnnotationView> views;
    views.reserve(annotations.size());
    for (const auto& a : annotations)
        if (a)
            views.push_back(annotationView(*a));
    return views;
}


ObjModuleType* annotationOwnerModule(ObjFunction* fn)
{
    if (!fn)
        return nullptr;
    Value mv = fn->moduleType.strongRef();
    return (mv.isObj() && isModuleType(mv)) ? asModuleType(mv) : nullptr;
}

const std::vector<ptr<ast::Annotation>>* declAnnotationNodes(ObjModuleType* mod,
                                                             const ustring& declName)
{
    if (!mod)
        return nullptr;
    auto it = mod->declAnnotations.find(declName.hashCode());
    return (it == mod->declAnnotations.end()) ? nullptr : &it->second;
}

bool hasAnnotation(ObjFunction* fn, const char* name)
{
    return fn && listHasAnnotation(fn->annotations, name);
}

bool hasAnnotation(ObjModuleType* mod, const ustring& declName, const char* name)
{
    const auto* nodes = declAnnotationNodes(mod, declName);
    return nodes && listHasAnnotation(*nodes, name);
}


std::vector<AnnotationView> declAnnotationsOf(ObjModuleType* mod, const ustring& declName)
{
    const auto* nodes = declAnnotationNodes(mod, declName);
    return nodes ? annotationViews(*nodes) : std::vector<AnnotationView>{};
}

std::vector<AnnotationView> annotationsOf(ObjFunction* fn)
{
    return fn ? annotationViews(fn->annotations) : std::vector<AnnotationView>{};
}


Value evalAnnotationArg(const ptr<ast::Expression>& e, ObjModuleType* owner, VM& vm,
                        GCNoParkCover& cover)
{
    (void)cover;   // a compile-time token: holding one is the whole contract
    return evalArg(e, owner, vm);
}

} // namespace roxal
