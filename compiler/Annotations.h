#pragma once

// Reading annotations off compiled Roxal declarations.
//
// Annotations survive compilation in two places, both of them data records
// reconstructed by readAnnotation() rather than a live parse tree:
//   - ObjFunction::annotations       -- for callables
//   - ObjModuleType::declAnnotations -- for top-level var/const/type
//                                       declarations, keyed by name hash
// This header is the supported way to read either.
//
// Annotations are STATIC metadata, and this API keeps them that way: an
// AnnotationView holds no Value, so reading one neither allocates a Roxal
// object nor re-enters the interpreter.  A caller may therefore read
// annotations from any thread at any time -- during module registration,
// before the VM is up -- and may keep the result in its own C++ state
// indefinitely, with no GC root, no no-park cover and no lifetime coupling to
// the VM.  This is what a native module (e.g. a robot module acting on
// `@joint(...) var elbow = 0`) wants.
//
// Two argument forms are deliberately left UNRESOLVED, because resolving them
// is what would drag the VM back in:
//
//   * a suffixed literal (2s, 100hz) is reported as its literal plus the
//     suffix text.  Evaluating it would call the @suffix-registered Roxal
//     function -- ordinary bytecode returning a `quantity` object (see
//     modules/sys.rox) -- which a native client would then have to take apart
//     again.  {100, "hz"} is both cheaper and more useful.
//
//   * a bare name (@cfunc(lib=cvxlib)) is reported as the identifier.  The
//     referenced module variable may hold anything, including a runtime
//     handle: `cvxlib` is a sys.loadlib() result.  A client that wants the
//     value does `mod->vars.load(name)` itself, which is exactly what the FFI
//     layer already does in FFI.cpp.
//
// The one surface that DOES evaluate arguments is the `inspect` module, whose
// Roxal-facing signatures()/members() promise evaluated values.  That path is
// at the bottom of this header and carries a GC cover; nothing else should
// need it.

#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <core/memory.h>
#include <core/types.h>

#include "SimpleMarkSweepGC.h"
#include "Value.h"

namespace roxal::ast {
    struct Annotation;
    struct Expression;
}

namespace roxal {

class VM;
struct ObjFunction;
struct ObjModuleType;


// ---- static argument model ------------------------------------------------

// One annotation argument value.  Mirrors the family the compiler admits (see
// RoxalCompiler::checkAnnotationArgs) and the module cache round-trips, with
// negated numbers already folded into Int/Real.
struct AnnotationArg {
    enum class Kind {
        Nil,
        Bool,
        Int,
        Real,
        String,
        Suffixed,   // a literal with a unit suffix: 2s, 100hz, '3'px
        Name,       // a bare identifier, NOT resolved
        List,
        Dict,
    };

    Kind kind { Kind::Nil };

    bool boolean { false };     // Bool
    int64_t integer { 0 };      // Int
    double real { 0.0 };        // Real
    ustring text;               // String, Name

    // Suffixed: `suffix` is the unit ("s", "hz"), and the literal it applies to
    // is the single element of `items` (an Int, Real or String).
    ustring suffix;

    std::vector<AnnotationArg> items;                              // List, Suffixed
    std::vector<std::pair<AnnotationArg, AnnotationArg>> entries;  // Dict, in source
                                                                   // order; keys may be
                                                                   // any literal form

    // The literal a Suffixed argument applies to.  Only valid when
    // kind == Kind::Suffixed.
    const AnnotationArg& suffixedLiteral() const { return items.at(0); }
};

// One annotation with its arguments in static form.
struct AnnotationView {
    ustring name;                                          // "df", without the '@'
    std::vector<AnnotationArg> args;                       // positional, in order
    std::vector<std::pair<ustring, AnnotationArg>> named;  // named, in source order

    // The named argument with this name, or nullptr.
    const AnnotationArg* arg(const ustring& argName) const;
};


// ---- the converter: a pure function of an AST node ------------------------
// Deliberately independent of where the annotation came from, so it also
// serves annotation sites that are parsed but not retained (imports, which
// keep only their names; parameters; type properties) and annotations read off
// a freshly parsed tree.  Throws std::runtime_error on an argument outside the
// admitted family, which a compiled annotation cannot contain.

AnnotationView annotationView(const ast::Annotation& annot);
std::vector<AnnotationView> annotationViews(const std::vector<ptr<ast::Annotation>>& annotations);


// ---- retrieval: pure lookups, no allocation -------------------------------

// The module that declared this callable, or nullptr.
ObjModuleType* annotationOwnerModule(ObjFunction* fn);

// The annotations on a top-level declaration of `mod`, or nullptr if it has
// none.  The pointee is owned by the module; it stays valid until the module is
// recompiled or dropped.
const std::vector<ptr<ast::Annotation>>* declAnnotationNodes(ObjModuleType* mod,
                                                             const ustring& declName);

// Presence tests, which never look at arguments.
bool hasAnnotation(ObjFunction* fn, const char* name);
bool hasAnnotation(ObjModuleType* mod, const ustring& declName, const char* name);


// ---- convenience: retrieval + conversion in one call ----------------------
// Derived from the two layers above, so they cannot drift from them.

// The annotations of a top-level var/const/type declaration.  Empty when the
// declaration has none (or does not exist).
std::vector<AnnotationView> declAnnotationsOf(ObjModuleType* mod, const ustring& declName);

// The annotations of a callable.
std::vector<AnnotationView> annotationsOf(ObjFunction* fn);


// ---- evaluating path: `inspect` only --------------------------------------
// Unlike everything above, this produces Values: it resolves bare names
// against the declaring module and calls the @suffix function for a suffixed
// literal, so it allocates and re-enters the VM.  It exists because
// inspect.signatures()/members() promise Roxal code evaluated arguments.
//
// It must run on the VM thread under a no-park cover, which it takes by
// non-const reference so the cover cannot be forgotten and cannot be a
// temporary that dies at the semicolon.  Under the cover no collection runs,
// so the returned Value is safe until the cover goes out of scope; to retain
// it beyond that, copy it into a typed persistent root (GCRoots.h).
//
//     SimpleMarkSweepGC::GCNoParkScope cover;
//     Value v = evalAnnotationArg(expr, owner, vm, cover);
//
// `owner` is the module the annotation was declared in; it may be nullptr, in
// which case only globals and sys's suffixes resolve.
using GCNoParkCover = SimpleMarkSweepGC::GCNoParkScope;

Value evalAnnotationArg(const ptr<ast::Expression>& expr, ObjModuleType* owner, VM& vm,
                        GCNoParkCover& cover);


// run static annotation-reading unit tests used by the runtime '_runtests'
// builtin (see compiler/annotations_test.cpp, tests/annotations_selftest.rox)
std::vector<std::tuple<std::string, bool, std::string>> testAnnotations();

} // namespace roxal
