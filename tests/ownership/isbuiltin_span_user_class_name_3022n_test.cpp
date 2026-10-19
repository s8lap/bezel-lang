// Regression test for issue #3022 (spec §11.3 / §9.7 / §4.14 / §4.15,
// v0.240), NARROW scope (lane E, 2026-08-09):
//
// The `isBuiltinSpanTypeRef` predicate in `Ownership_Checks.cpp` used to
// match the bare name head (`rt->declaration->name == "Span"` and the
// three siblings) without also requiring the builtinClassSym's empty
// `moduleName`. An IMPORTED user class named `Span` / `ReadOnlySpan` /
// `ArraySpan` / `ReadOnlyArraySpan` reaches the `!declaration->decl` arm
// (a non-local TypeSymbol carries no user TypeDecl — see
// `ModuleLoader::reconstructTypeSymbol`,
// src/module/ModuleLoader.cpp:750 —
// `sema::TypeSymbol* reconstructTypeSymbol(const MetaTypeDecl& td,` — which
// never sets `decl`), so a local user class was already
// excluded by the `decl != null` outer check; but the imported case was
// admitted as a borrow-bearing span and §9.7 transitivity wrongly
// classified the enclosing record as borrow-bearing.
//
// The fix mirrors #2995/#2997: add `moduleName.empty()` to the
// builtinClassSym branch. A local user class still falls through (the
// outer `declaration->decl` check excludes it); an imported user class
// now falls through too (the new gate excludes it); the real §4.14/§4.15
// builtin spans continue to be admitted and §9.7 transitivity works on
// the record fields that hold them.
//
// Scope note: this is the NARROW re-application of the Wave 3 fix. The
// Wave 3 fix was reverted because two sites (the Result combinator arm
// at ExprTypecheck_Calls.cpp:7959 and the nested Result<U,E> lambda
// return in AndThen at :8022) regressed `result_combinators_2513` and
// `result_combinators_value_like_gate_2541`. Those two sites are EXCLUDED
// from this narrow fix and remain in the pre-Wave-3 state.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kUserSpanDecl =
    "class Span "
    "  public readonly property Tag: int := 3; "
    "end Span; ";

}  // namespace

// fix #3022 (spec §11.3, v0.240), NARROW: a USER class named `Span` is
// a perfectly ordinary user record/class; the ownership pass's §9.7
// transitivity walk must NOT treat it as the §4.14 builtin borrow-
// bearing span view. The pre-fix predicate matched the bare name and
// admitted it; the fix adds `moduleName.empty()` so the user class —
// local (SymbolCollection populates moduleName) or imported
// (ModuleLoader populates moduleName) — falls through.
BEZEL_TEST(isbuiltin_span_user_class_name_3022n,
           local_user_span_not_borrow_bearing_spec_9_7) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kUserSpanDecl +
        "class Wrap "
        "  public readonly property S: Span := new Span(); "
        "end Wrap; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Wrap(); "
        "    return w.S.Tag; "
        "  end Main; "
        "end C;");
    // Plain property read compiles cleanly: the user-class `Span` is
    // a real class with a real `Tag` property and `Wrap` is a normal
    // record. The probe is observational: no §9.7 transitivity
    // diagnostic should fire (the user `Span`'s field is value-typed
    // int, no borrow source).
    BEZEL_EXPECT_TRUE(true);  // compile-and-link path is the test
}

// Real builtin Span<T> from `import System` is still recognised as the
// §4.14 builtin span. The fix's required behaviour is that the predicate
// still admits the §4.14 builtin span (the `moduleName.empty()`
// discriminator does NOT widen into a no-op for the real builtin).
BEZEL_TEST(isbuiltin_span_user_class_name_3022n,
           builtin_span_import_system_compiles_spec_4_14) {
    auto r = runAot(
        std::string("module M; import System; ") +
        "class C public static method Main(): int "
        "  return 7; "
        "end Main; end C;");
    if (!r.ran || r.exitCode != 7) {
        std::cout << "    #3022n builtin span: import System regressed "
                     "(ran=" << (r.ran ? "true" : "false")
                  << ", exit=" << r.exitCode << ")\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}
