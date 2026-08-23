/* Static annotation reading (compiler/Annotations.h).

   A C++ test because the API under test is C++: it converts ast::Annotation
   nodes into AnnotationViews with no Value, no VM and no GC involved, so there
   is nothing for a .rox test to call.  The nodes are built by hand rather than
   parsed, which also pins the contract that conversion is a pure function of a
   node -- it takes no module and no VM, so it works on annotations from any
   source, including sites that are never retained (imports, parameters, type
   properties) and freshly parsed trees.

   The evaluating path, which does need the VM and a GC cover, is covered from
   Roxal by tests/inspect_signatures.rox and tests/inspect_var_annotations.rox.

   Invoked via: _runtests('annotations')
   Returns results in the standard _runtests tuple format.
*/

#include "Annotations.h"

#include <core/AST.h>

#include <string>
#include <tuple>
#include <vector>

using roxal::AnnotationArg;
using roxal::AnnotationView;
using roxal::ptr;
using roxal::ustring;
namespace ast = roxal::ast;

namespace roxal {

namespace {

using Results = std::vector<std::tuple<std::string, bool, std::string>>;

Results* results = nullptr;

void check(bool condition, const std::string& name)
{
    results->emplace_back(name, condition, condition ? "ok" : "MISMATCH");
}

ptr<ast::Expression> str(const char* s)
{
    auto n = roxal::make_ptr<ast::Str>();
    n->str = roxal::toUnicodeString(s);
    return n;
}

ptr<ast::Expression> num(int32_t v)
{
    auto n = roxal::make_ptr<ast::Num>();
    n->num = v;
    return n;
}

ptr<ast::Expression> real(double v)
{
    auto n = roxal::make_ptr<ast::Num>();
    n->num = v;
    return n;
}

ptr<ast::Expression> boolean(bool v)
{
    auto n = roxal::make_ptr<ast::Bool>();
    n->value = v;
    return n;
}

ptr<ast::Expression> nil()
{
    auto n = roxal::make_ptr<ast::Literal>();
    n->literalType = ast::Literal::LiteralType::Nil;
    return n;
}

ptr<ast::Expression> name(const char* s)
{
    auto n = roxal::make_ptr<ast::Variable>();
    n->name = roxal::toUnicodeString(s);
    return n;
}

ptr<ast::Expression> suffixedNum(int32_t v, const char* suffix)
{
    auto n = roxal::make_ptr<ast::SuffixedNum>();
    n->num = v;
    n->suffix = roxal::toUnicodeString(suffix);
    return n;
}

ptr<ast::Expression> negate(ptr<ast::Expression> arg)
{
    auto n = roxal::make_ptr<ast::UnaryOp>(ast::UnaryOp::Negate);
    n->arg = arg;
    return n;
}

ptr<ast::Expression> list(std::vector<ptr<ast::Expression>> elements)
{
    auto n = roxal::make_ptr<ast::List>();
    n->elements = std::move(elements);
    return n;
}

ptr<ast::Expression> dict(std::vector<std::pair<ptr<ast::Expression>, ptr<ast::Expression>>> entries)
{
    auto n = roxal::make_ptr<ast::Dict>();
    n->entries = std::move(entries);
    return n;
}

// @<name>(...) with the given (argName, value) pairs; an empty argName is positional.
ptr<ast::Annotation> annotation(const char* annotName,
                                std::vector<std::pair<const char*, ptr<ast::Expression>>> args)
{
    auto a = roxal::make_ptr<ast::Annotation>();
    a->name = roxal::toUnicodeString(annotName);
    for (auto& arg : args)
        a->args.emplace_back(arg.first ? roxal::toUnicodeString(arg.first) : ustring(),
                             arg.second);
    return a;
}

std::string utf8(const ustring& s) { return roxal::toUTF8StdString(s); }


void testScalars()
{
    // @meta('a var', -7, 2.5, true, nil, SCALE)
    auto a = annotation("meta", {
        {nullptr, str("a var")},
        {nullptr, negate(num(7))},
        {nullptr, real(2.5)},
        {nullptr, boolean(true)},
        {nullptr, nil()},
        {nullptr, name("SCALE")},
    });

    AnnotationView v = roxal::annotationView(*a);
    check(utf8(v.name) == "meta", "annotation name");
    check(v.args.size() == 6, "positional argument count");
    check(v.named.empty(), "no named arguments");

    check(v.args[0].kind == AnnotationArg::Kind::String && utf8(v.args[0].text) == "a var",
          "string argument");
    // a unary minus is folded into the literal: clients never see a negation node
    check(v.args[1].kind == AnnotationArg::Kind::Int && v.args[1].integer == -7,
          "negated int argument folds to Int");
    check(v.args[2].kind == AnnotationArg::Kind::Real && v.args[2].real == 2.5,
          "real argument");
    check(v.args[3].kind == AnnotationArg::Kind::Bool && v.args[3].boolean,
          "bool argument");
    check(v.args[4].kind == AnnotationArg::Kind::Nil, "nil argument");
    // a bare name stays UNRESOLVED -- resolving it is what would need the VM
    check(v.args[5].kind == AnnotationArg::Kind::Name && utf8(v.args[5].text) == "SCALE",
          "bare name stays unresolved");
}

void testNamedArguments()
{
    // @df(x=10, y=-5.5)
    auto a = annotation("df", { {"x", num(10)}, {"y", negate(real(5.5))} });

    AnnotationView v = roxal::annotationView(*a);
    check(v.args.empty(), "no positional arguments");
    check(v.named.size() == 2, "named argument count");
    // source order is preserved, not sorted
    check(utf8(v.named[0].first) == "x" && utf8(v.named[1].first) == "y",
          "named arguments keep source order");

    const AnnotationArg* x = v.arg(roxal::toUnicodeString("x"));
    const AnnotationArg* y = v.arg(roxal::toUnicodeString("y"));
    check(x && x->kind == AnnotationArg::Kind::Int && x->integer == 10, "arg('x')");
    check(y && y->kind == AnnotationArg::Kind::Real && y->real == -5.5, "arg('y')");
    check(v.arg(roxal::toUnicodeString("nope")) == nullptr, "arg() of a missing name");
}

void testSuffixed()
{
    // @rate(100hz, -2s) -- reported as literal + suffix, NOT evaluated into a
    // quantity object (which would mean running Roxal code)
    auto a = annotation("rate", {
        {nullptr, suffixedNum(100, "hz")},
        {nullptr, negate(suffixedNum(2, "s"))},
    });

    AnnotationView v = roxal::annotationView(*a);
    check(v.args[0].kind == AnnotationArg::Kind::Suffixed, "suffixed kind");
    check(utf8(v.args[0].suffix) == "hz", "suffix text");
    check(v.args[0].suffixedLiteral().kind == AnnotationArg::Kind::Int
              && v.args[0].suffixedLiteral().integer == 100,
          "suffixed literal");
    // negation reaches through the suffix to the literal underneath
    check(utf8(v.args[1].suffix) == "s" && v.args[1].suffixedLiteral().integer == -2,
          "negated suffixed literal");
}

void testNestedCollections()
{
    // @someannot(q=[1,2,3], r={'l': [3, {'m':'n'}]})
    auto a = annotation("someannot", {
        {"q", list({num(1), num(2), num(3)})},
        {"r", dict({ {str("l"), list({num(3), dict({ {str("m"), str("n")} })})} })},
    });

    AnnotationView v = roxal::annotationView(*a);

    const AnnotationArg* q = v.arg(roxal::toUnicodeString("q"));
    check(q && q->kind == AnnotationArg::Kind::List && q->items.size() == 3, "list argument");
    check(q && q->items[2].integer == 3, "list element");

    const AnnotationArg* r = v.arg(roxal::toUnicodeString("r"));
    check(r && r->kind == AnnotationArg::Kind::Dict && r->entries.size() == 1,
          "dict argument");
    if (r && r->entries.size() == 1) {
        const AnnotationArg& key = r->entries[0].first;
        const AnnotationArg& val = r->entries[0].second;
        check(key.kind == AnnotationArg::Kind::String && utf8(key.text) == "l", "dict key");
        check(val.kind == AnnotationArg::Kind::List && val.items.size() == 2,
              "nested list in dict");
        if (val.items.size() == 2) {
            const AnnotationArg& inner = val.items[1];
            check(inner.kind == AnnotationArg::Kind::Dict && inner.entries.size() == 1,
                  "dict nested inside a list");
            if (inner.entries.size() == 1)
                check(utf8(inner.entries[0].second.text) == "n", "innermost value");
        }
    }
}

void testList()
{
    std::vector<ptr<ast::Annotation>> annots {
        annotation("a", {}),
        nullptr,                                  // skipped, not crashed on
        annotation("b", { {nullptr, num(1)} }),
    };
    auto views = roxal::annotationViews(annots);
    check(views.size() == 2, "null annotations are skipped");
    check(utf8(views[0].name) == "a" && utf8(views[1].name) == "b", "list order");
    check(views[0].args.empty(), "annotation with no arguments");
}

void testMalformed()
{
    // The compiler admits Negate of any admitted argument, so '-"x"' can be
    // written; the converter must report rather than produce nonsense.
    auto a = annotation("bad", { {nullptr, negate(str("x"))} });
    try {
        roxal::annotationView(*a);
        check(false, "negating a string should raise");
    } catch (const std::runtime_error&) {
        // expected
    }
}

} // anonymous namespace

std::vector<std::tuple<std::string, bool, std::string>> testAnnotations()
{
    Results out;
    results = &out;
    testScalars();
    testNamedArguments();
    testSuffixed();
    testNestedCollections();
    testList();
    testMalformed();
    results = nullptr;
    return out;
}

} // namespace roxal
