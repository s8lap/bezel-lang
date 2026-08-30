// Spec-derived regression tests for issue #3635 — §6.1 / Appendix C.11's
// const-initializer overflow check silently did not fire when the
// initializer referenced an INHERITED const by bare name.
//
// MECHANISM: `Resolver::checkTypeConsts` (TypeResolution_Checks.cpp) supplies
// the `resolve(name, depth)` closure `foldIntConst`/`containsConstDivByZero`
// call to fold a class-level const's initializer. Before this fix that
// closure walked ONLY `t->members` (the one type currently being validated) —
// no class base-chain walk. So for `Derived : Base` with `Derived`'s own
// const initializer referencing `Base`'s by bare name, `resolve` returned
// nullopt, `foldIntConst` returned nullopt, and the ONLY nullopt-arm
// diagnostic that existed (division-by-zero, §22.12) did not apply — the
// §6.1 / Appendix C.11 range check silently no-op'd. A character-identical
// same-class twin (both consts on ONE class) was correctly rejected, proving
// the gap was scope, not the rule itself.
//
// `fix #3619` had already collapsed the OTHER two Resolver-side const-ident
// resolvers (which DO walk the base chain) into `resolveConstIdentInScope`,
// but explicitly DECLINED to fold `checkTypeConsts`'s closure into that
// merge: `resolveConstIdentInScope` falls through past an unfoldable name
// match to let a same-named symbol further up shadow it, while
// `checkTypeConsts`'s closure returns immediately on ANY name match, foldable
// or not. This fix preserves that early-return contract — it widens WHICH
// levels of the chain get searched (t's own members, then its class base
// chain, matching `resolveConstIdentInScope`'s walk), not WHAT HAPPENS on a
// match.
//
// A SECOND, independently-reachable instance of the identical bug was found
// while re-verifying this fix: `containsConstDivByZero` (the nullopt arm's
// OTHER existing diagnostic) shares the exact same `resolve` closure, so a
// class-level const dividing by an INHERITED zero const was ALSO silently
// accepted pre-fix (no test in tree covered it). Both symptoms share one
// root cause and one fix, pinned together below.
//
// ARRAY-SIZE SHAPE — checked, NOT part of this bug. `narrow #3617` measured
// an IMPORTED const used as `[N]T` emitting malformed IR because a DIFFERENT
// resolver family (the one #3619 already fixed: `foldIntConstExpr` /
// `resolveConstIdentInScope`, reached from `TypeResolution_Names.cpp`'s
// `[N]T` fold and from `ExprTypecheck.cpp`'s local-const array-size fold)
// lacked #3230's imported-const fallback. `checkTypeConsts` is NOT in that
// call path at all — it only validates `const` MEMBER DECLARATIONS' own
// overflow, never `[N]T` sizes. Measured directly: an inherited-but-LOCAL
// const (`Base.K`) used to size a `[n]int` from a `Derived` method already
// compiled and ran correctly on the PRE-fix tree (verified below,
// `inherited_local_const_sizes_array_unaffected_by_3635_spec_15_1`), because
// it goes through the already-fixed family, never through
// `checkTypeConsts::resolve`. Pinned here so a future change to either family
// cannot silently reopen the gap without failing a test.
//
// REGRESSION-TEST POLICY (regression-test-pins-the-bug): the `_defect_`-named
// tests below were ACCEPTED (rc=0, no diagnostic) before the fix; the
// `_control_`-named tests were rejected/accepted identically before and
// after (proving the fix neither narrows nor widens the unaffected shapes).
//
// MECHANICALLY VERIFIED fail-without/pass-with (this lane, F4c3): built
// bezelc with the fix, confirmed every `_defect_` .bzl repro below rejects
// with the exact diagnostic text asserted; reverted
// src/sema/TypeResolution_Checks.cpp to the pre-fix `checkTypeConsts`,
// rebuilt, and confirmed every `_defect_` repro instead compiled clean
// (rc=0, no diagnostic) — then restored the fix and rebuilt again, relinking
// before any further measurement.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// True iff the compile was rejected (not merely "some diagnostic exists" —
// `hasErrors()` gates it) AND some reported diagnostic's message contains
// `needle`.
bool hasMsg(const std::string& prog, const std::string& needle) {
    auto r = runAot(prog);
    if (r.ran) return false;              // compiled clean -- no diagnostic
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr const char* kOverflowDiag =
    "does not fit its declared type";
constexpr const char* kDivZeroDiag =
    "initializer contains a division or modulo by zero";

}  // namespace

// =============================================================================
// CONTROL — the same-class twin. Both before and after the fix this must be
// rejected: `checkTypeConsts` already found `SoloMax` in `t->members` without
// needing a base-chain walk. Pins that the fix did not change the unaffected
// (non-inherited) shape.
// =============================================================================
BEZEL_TEST(const_overflow_inherited_3635,
           same_class_const_overflow_rejected_spec_6_1_appendix_c_11) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class Solo "
        "  public const SoloMax: int := 100; "
        "  public const Bad: int := SoloMax + 2147483647; "
        "  public static method Main(): int return 0; end Main; "
        "end Solo;",
        kOverflowDiag));
}

// =============================================================================
// THE DEFECT (#3635's own repro) — `Derived : Base`, `Derived.Bad` overflows
// via `Base.BaseMax` referenced by bare name. Pre-fix: ACCEPTED SILENTLY
// (rc=0, no diagnostic). Post-fix: rejected with the exact §6.1/App C.11
// overflow text.
// =============================================================================
BEZEL_TEST(const_overflow_inherited_3635,
           inherited_const_overflow_rejected_spec_6_1_appendix_c_11) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class Base "
        "  public const BaseMax: int := 100; "
        "end Base; "
        "public class Derived : Base "
        "  public const Bad: int := BaseMax + 2147483647; "
        "  public static method Main(): int return 0; end Main; "
        "end Derived;",
        kOverflowDiag));
}

// A TWO-LEVEL chain (`Child : Parent : Grandparent`) referencing the
// GRANDPARENT's const by bare name — pins that the base-chain walk does not
// stop at the immediate base.
BEZEL_TEST(const_overflow_inherited_3635,
           inherited_const_overflow_two_level_chain_rejected_spec_6_1) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class Grandparent "
        "  public const G: int := 100; "
        "end Grandparent; "
        "public class Parent : Grandparent "
        "end Parent; "
        "public class Child : Parent "
        "  public const Bad: int := G + 2147483647; "
        "  public static method Main(): int return 0; end Main; "
        "end Child;",
        kOverflowDiag));
}

// =============================================================================
// THE SECOND SYMPTOM (found while re-verifying this fix, not in the filed
// issue) — the same narrow `resolve` closure backs `containsConstDivByZero`
// in the SAME nullopt arm. A class-level const dividing by an INHERITED zero
// const was ALSO silently accepted pre-fix. One root cause, one fix, pinned
// alongside the filed defect.
// =============================================================================
BEZEL_TEST(const_overflow_inherited_3635,
           inherited_const_div_by_zero_rejected_spec_22_12) {
    BEZEL_EXPECT_TRUE(hasMsg(
        "module M; "
        "public class Base "
        "  public const Z: int := 0; "
        "end Base; "
        "public class Derived : Base "
        "  public const Bad: int := 10 / Z; "
        "  public static method Main(): int return 0; end Main; "
        "end Derived;",
        kDivZeroDiag));
}

// =============================================================================
// OVER-REJECTION CONTROL — a legitimate inherited-const reference that does
// NOT overflow must still compile and run. A fix that rejected every
// inherited-const reference (rather than correctly folding and range-
// checking it) would pass every test above and fail this one. `3 + 4 = 7`.
// =============================================================================
BEZEL_TEST(const_overflow_inherited_3635,
           inherited_const_in_range_accepted_and_runs_spec_6_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public const K: int := 3; "
        "end Base; "
        "public class Derived : Base "
        "  public const Good: int := K + 4; "
        "  public static method Main(): int return Good; end Main; "
        "end Derived;"),
        7);
}

// =============================================================================
// ARRAY-SIZE SHAPE — NOT part of this bug (see file header). An
// inherited-but-LOCAL const sizing a `[n]int` from a `Derived` method takes a
// DIFFERENT resolver family (already fixed by #3619/#3230) and is unaffected
// by `checkTypeConsts`'s gap either before or after this fix. `7+8+9=24`.
// =============================================================================
BEZEL_TEST(const_overflow_inherited_3635,
           inherited_local_const_sizes_array_unaffected_by_3635_spec_15_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Base "
        "  public const K: int := 3; "
        "end Base; "
        "public class Derived : Base "
        "  public static method Main(): int "
        "    const n: int := K; "
        "    var a: [n]int := [7, 8, 9]; "
        "    return a[0] + a[1] + a[2]; "
        "  end Main; "
        "end Derived;"),
        24);
}
