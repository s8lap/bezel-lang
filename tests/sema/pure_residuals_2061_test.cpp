// Spec-derived tests for `pure` member residuals deferred from #2052 —
// issue #2061 IMPLEMENTATION items 1 (chain-purity for pure ctors) and 2
// (synthesized record-member pure-classification + record-construction
// purity).
//
// Spec §22.15 "Pure members" (v0.202, proposal 0042) — "Pure constructors"
// and "Members classified by rule" + Appendix C.14. Each test names the
// rule it pins a regression on.
//
// Items 3-6 in #2061 (default-value expressions, first-class-dereference
// path-classification precision, stdlib annotation, cross-module pure
// carriage) are intentionally DEFERRED — out of scope here.

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

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True iff some reported diagnostic's message contains BOTH `needle` and
// `other`. Used to narrow a chain-purity report to the actual `base(...)`
// or `this(...)` form.
bool hasMsgWithBoth(const bezel::test::SemaResult& r,
                    const std::string& needle, const std::string& other) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find(other) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =========================================================================
// §22.15 "Pure constructors" (#2061, item 1) — chain-purity rule.
// A pure constructor's base(...)/this(...) chain target must itself be pure.
// =========================================================================

// §22.15: a pure ctor with a `base(...)` chain to a NON-pure base ctor —
// REJECTED. The diagnostic names the rule AND the offending chain form.
BEZEL_TEST(pure_residuals_2061, pure_ctor_impure_base_chain_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor(b: int) end constructor; "
        "end Base; "
        "class D : Base "
        "  public pure constructor(d: int) "
        "    base(d); "
        "  end constructor; "
        "end D;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "constructor chain call"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "is not pure"));
    BEZEL_EXPECT_TRUE(hasMsgWithBoth(r, "constructor chain call", "base("));
}

// §22.15 (control): a pure ctor with a `base(...)` chain to a PURE base
// ctor — accepted, no diagnostic.
BEZEL_TEST(pure_residuals_2061, pure_ctor_pure_base_chain_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public pure constructor(b: int) end constructor; "
        "end Base; "
        "class D : Base "
        "  public pure constructor(d: int) "
        "    base(d); "
        "  end constructor; "
        "end D;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "constructor chain call"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure ctor with a `this(...)` chain to a NON-pure sibling ctor
// in the same class — REJECTED, diagnostic names the form.
BEZEL_TEST(pure_residuals_2061, pure_ctor_impure_this_chain_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor(x: int) end constructor; "
        "  public pure constructor() "
        "    this(0); "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "constructor chain call"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "is not pure"));
    BEZEL_EXPECT_TRUE(hasMsgWithBoth(r, "constructor chain call", "this("));
}

// §22.15 (control): a pure ctor with a `this(...)` chain to a PURE sibling
// ctor — accepted.
BEZEL_TEST(pure_residuals_2061, pure_ctor_pure_this_chain_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure constructor(x: int) end constructor; "
        "  public pure constructor() "
        "    this(0); "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "constructor chain call"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a pure ctor with NO explicit chain — accepted (the implicit
// `base()` calls Object's trivial ctor, which is pure; the rule keys on
// explicit chain targets only).
BEZEL_TEST(pure_residuals_2061, pure_ctor_no_explicit_chain_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public pure constructor(x: int) "
        "    var y: int := x; "
        "    y := y + 1; "
        "  end constructor; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "constructor chain call"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: the chain-purity rule fires ONLY for pure ctors. A non-pure ctor
// with an impure base chain — NOT flagged by the chain rule (the rule is
// scoped to pure ctors; non-pure ctors are not constrained by §22.15 at
// all).
BEZEL_TEST(pure_residuals_2061,
           nonpure_ctor_impure_base_chain_not_flagged_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Base "
        "  public constructor(b: int) end constructor; "
        "end Base; "
        "class D : Base "
        "  public constructor(d: int) "
        "    base(d); "
        "  end constructor; "
        "end D;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "constructor chain call"));
}

// =========================================================================
// §22.15 "Members classified by rule" (#2061, item 2) — synthesized record
// members (Equals/GetHashCode/ToString) are pure-classified iff the record
// class is auto-equality-bearing (every field's dispatch target is pure).
// =========================================================================

// §22.15: a call from a pure body to a positional record's synthesized
// `GetHashCode` — admitted when the record's fields are pure-admissible
// (every int field is pure under §22.15 rules 1+3+6).
BEZEL_TEST(pure_residuals_2061,
           pure_calls_positional_record_gethashcode_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(x: int, y: int); "
        "class C "
        "  public pure method M(r: R): int "
        "    return r.GetHashCode(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a call from a pure body to a positional record's synthesized
// `Equals` — admitted when the record's fields are pure-admissible.
BEZEL_TEST(pure_residuals_2061,
           pure_calls_positional_record_equals_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(x: int, y: int); "
        "class C "
        "  public pure method M(a: R, b: R): bool "
        "    return a.Equals(b); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a call from a pure body to a positional record's synthesized
// `ToString` — admitted when the record's fields are pure-admissible
// (every field's `ToString` is pure under the field-type check).
BEZEL_TEST(pure_residuals_2061,
           pure_calls_positional_record_tostring_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(x: int, y: int); "
        "class C "
        "  public pure method M(r: R): string "
        "    return r.ToString(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a call from a pure body to a body record's synthesized
// `GetHashCode` — admitted when the body record is auto-equality-bearing
// (every auto-property is a pure-admissible component type — the
// auto-equality-bearing form uses `public property X: T;` declarations).
BEZEL_TEST(pure_residuals_2061,
           pure_calls_body_record_gethashcode_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property X: int; "
        "  public property Y: string; "
        "  public constructor(x: int, y: string) "
        "    this.X := x; this.Y := y; "
        "  end constructor; "
        "end R; "
        "class C "
        "  public pure method M(r: R): int "
        "    return r.GetHashCode(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a non-positional, non-equality-bearing record — synthesized
// members exist but the synthesized body's purity is not provable (a
// class-typed field's virtual `Equals` disqualifies). The pure-body
// checker reports a §22.15 violation. The sema-level §9.2/§9.3 separate
// check (which rejects `.Equals` on a non-equality-bearing record) also
// fires — either diagnostic is sufficient.
BEZEL_TEST(pure_residuals_2061,
           pure_calls_non_equality_bearing_record_gethashcode_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "class K destructor end destructor; end K; "
        "record R(k: K, n: int); "
        "class C "
        "  public pure method M(r: R): int "
        "    return r.GetHashCode(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =========================================================================
// §22.15 "Members classified by rule" (#2061, item 2) — record construction
// `new R(args)` from a pure body. Admitted for positional records whose
// synthesized ctor is pure-classified (record is auto-equality-bearing) and
// for body records with a `pure`-declared ctor.
// =========================================================================

// §22.15: positional record construction from a pure body — admitted
// (positional records' synthesized positional ctors are pure-classified
// when the record is auto-equality-bearing; here every field is an int,
// which is always admissible, so the classification is pure).
BEZEL_TEST(pure_residuals_2061,
           pure_body_constructs_positional_record_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(x: int, y: int); "
        "class C "
        "  public pure method M(): R "
        "    return new R(1, 2); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a body record with a `pure`-declared ctor — construction from
// a pure body admitted. The ctor's arg-copying birth stores are pure
// (this is the canonical pure-ctor form, just on a record instead of a
// class). The auto-property storage is set via an explicit `this.V := v;`
// so the assignment's target root is `this` (a pure-ctor birth store).
BEZEL_TEST(pure_residuals_2061,
           pure_body_constructs_body_record_pure_ctor_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property V: int := 0; "
        "  public pure constructor(v: int) "
        "    this.V := v; "
        "  end constructor; "
        "end R; "
        "class C "
        "  public pure method M(): R "
        "    return new R(7); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.15: a body record with a NON-pure user-declared ctor — construction
// from a pure body is REJECTED with the §22.15 record-construction
// diagnostic. The body record has no synthesized ctor (no positional
// fields); the invoked user-declared ctor is non-pure, so the
// construction is not pure-classified.
BEZEL_TEST(pure_residuals_2061,
           pure_body_constructs_body_record_impure_ctor_rejected_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R "
        "  public property V: int := 0; "
        "  public constructor(v: int) "
        "    this.V := v; "
        "  end constructor; "
        "end R; "
        "class C "
        "  public pure method M(): R "
        "    return new R(7); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMsg(r, "record construction"));
}

// §22.15: a positional record's user-declared pure ctor of matching
// arity (1 here) shadows the synthesized arity-1 positional ctor; the
// construction is admitted because the user's ctor carries `pure`.
// The class-identity of the record still determines purity — the user's
// pure ctor just supplies the body.
BEZEL_TEST(pure_residuals_2061,
           pure_body_constructs_positional_record_pure_user_ctor_accepted_spec_22_15) {
    auto r = parseSemaAll(
        "module M; "
        "record R(x: int) "
        "  public pure constructor(x: int) "
        "    begin end; "
        "  end constructor; "
        "end R; "
        "class C "
        "  public pure method M(): R "
        "    return new R(1); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasMsg(r, "record construction"));
}
