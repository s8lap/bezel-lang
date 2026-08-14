// Spec-derived tests for the §22.15 "Pure constructors" birth-store rule
// when the target root is a PROPERTY (auto-property backing store) and the
// LHS is the bare-identifier spelling — issue #3208.
//
// Spec §22.15 "Pure constructors" (v0.254):
//
//   A constructor may be declared `pure`. Its body is checked under the
//   pure-body rules with one addition, the **birth-store rule**:
//   assignments whose target root is a field of `this` (including
//   `readonly` auto-property backing stores) are permitted — the instance
//   under construction is unreachable from any other holder until the
//   constructor returns, so its initialization is not an observable
//   effect.
//
// The rule is stated over the TARGET ROOT, not over its spelling. The
// bare-identifier spelling `N := n;` and the `this.`-qualified spelling
// `this.N := n;` name the same store (§22.9) and the birth-store rule
// must admit both. Before #3208, the implementation admitted only the
// `this.`-qualified spelling (the bare-identifier spelling resolved to
// a PropertySymbol and fell through to the "not a frame-local" reject),
// so a §22.7/§22.8-shaped `pure` constructor on the idiomatic spelling
// was a false REJECT.
//
// The fixture rows pin the spec rule as four-row regression matrix:
//   T1  bare property write in a pure class ctor        -> ADMITTED
//   T2  `this.`-qualified property write in a pure ctor -> ADMITTED (the
//       pre-fix partner that kept the asymmetric spellings compiling
//       before #3208's predicate was widened)
//   T3  bare property write in a pure METHOD (not ctor) -> REJECTED
//       (the differential control that makes T1 a birth-store claim,
//       not a generic "property writes are admitted" claim)
//   T4  static property write in a pure ctor             -> REJECTED
//       (the qualifier-irrelevance extension preserves the static-field
//       carve-out from the existing FieldSymbol branch — a store to a
//       static property is just as effect-visible as a store to a static
//       field, and the existing diagnostic is reused verbatim)
//
// The matrix is statically resolvable; the test uses the SemaResult
// harness (parseSemaAll) so the rows are paired fail-rows / pass-rows
// rather than full AOT compiles — the spec's predicate is a
// sema-resolvable fact, not a codegen one.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §22.15 birth-store rule (v0.254, fix #3208): a pure class ctor may
// write to a property of `this` directly — the bare-identifier spelling
// `N := n;` is the same store as `this.N := n;` and the birth-store rule
// admits both. The diagnostic from the previous implementation named
// "frame-local", which is the wrong rule — the birth-store rule is the
// EXCEPTION to frame-locality.
BEZEL_TEST(pure_ctor_birth_store_property_3208,
           bare_property_write_in_pure_ctor_admitted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property N: int := 0; "
        "  public pure constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 birth-store rule (control row): the `this.`-qualified spelling
// was admitted before #3208 — this row keeps the existing surface pinned
// so the fix doesn't regress the partner spelling.
BEZEL_TEST(pure_ctor_birth_store_property_3208,
           qualified_property_write_in_pure_ctor_admitted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property N: int := 0; "
        "  public pure constructor(n: int) "
        "    this.N := n; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15 birth-store rule (control row): a property write in a pure
// METHOD (not a constructor) is NOT a birth-store — the instance is
// fully constructed and reachable, so the property write is an
// observable effect. The fix must not over-widen the predicate. The
// assertion is the closed form ("assignment is not admitted in a pure
// body") rather than the new wording on the property branch, so the row
// pins the predicate's negative side across the diagnostic-text change.
BEZEL_TEST(pure_ctor_birth_store_property_3208,
           bare_property_write_in_pure_method_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public property P: int := 0; "
        "  public pure method Reset(): int "
        "    P := 7; "
        "    return 0; "
        "  end Reset; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasMsg(r, "assignment is not admitted in a pure body"));
}

// §22.15 birth-store rule (control row): a STATIC property write in a
// pure ctor is NOT a birth-store — the static property's storage is
// process-global, not "unreachable from any other holder until the
// constructor returns". The fix preserves the FieldSymbol arm's
// static-carve-out by checking the same `modifiers` list on the
// PropertySymbol. Same closed-form assertion as the preceding row.
BEZEL_TEST(pure_ctor_birth_store_property_3208,
           static_property_write_in_pure_ctor_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static property S: int := 0; "
        "  public pure constructor(n: int) "
        "    S := n; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(
        hasMsg(r, "assignment is not admitted in a pure body"));
}

// §22.15 birth-store rule (record row): a record's compiled positional
// field folded into an auto-property is a "readonly auto-property backing
// store" per the spec — the same admission as a class property write.
// The record constructor spelling lets the bare form `X := x;` go
// through the same PropertySymbol branch on the same rationale. Row
// kept on a record to pin the record-path parity, not just the
// class-path one.
BEZEL_TEST(pure_ctor_birth_store_property_3208,
           bare_property_write_in_pure_record_ctor_admitted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property X: int; "
        "  public pure constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end R;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
