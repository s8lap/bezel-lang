// Spec-derived tests for issue #1756 — spec §22.3 "ref and out argument
// locations" (v0.226): a `ref`/`out` argument aliases the caller's storage
// location DIRECTLY, so its type must EXACTLY match the parameter's referent
// type — no §5.1 implicit conversion is admitted (the same "aliases the
// caller's location, so no conversion is admitted" principle §14.5 states for
// the generic-inference case).
//
// ExprTC::checkArgsAgainstParams unconditionally skipped EVERY modifier-bearing
// argument, so the concrete (non-generic) ref/out position was never
// type-checked: a `ref s` of type `long` against a `ref slot: int` parameter
// compiled clean and aliased mismatched-width storage through the parameter
// (the issue's repro). The generic counterpart is §14.5's domain
// (checkGenericRefOutExactUnification, #1686); this check is NON-GENERIC ONLY.
//
// The two NEGATIVE tests below fail WITHOUT the fix (the call compiled clean,
// so hasErrors() is false) and pass WITH it (the §22.3-cited exact-type
// diagnostic fires). The POSITIVE test pins the matching-type case to guard
// against over-rejection: prior #1756 reworks repeatedly broke valid generic
// ref/out calls, so an exact-match non-generic control must stay green.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True when one of the result's diagnostics contains `needle`. The §22.3
// exact-type diagnostic is a SEMA message, so it surfaces through `runAot`'s
// diagnostics engine.
bool diagContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =============================================================================
// NEGATIVE — a ref/out argument whose type differs from the parameter's
// referent type must be rejected with the §22.3 exact-type diagnostic. These
// fail WITHOUT the fix (no check ran → clean compile → hasErrors() false).
// =============================================================================

// §22.3 (#1756): a `ref s` of type `long` against a `ref slot: int` parameter
// is not an exact match — the argument aliases mismatched-width storage through
// the parameter. Pre-fix this compiled clean.
BEZEL_TEST(ref_out_arg_type_match_1756, ref_mismatch_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Store(ref slot: int) slot := 99; end Store; "
        "  public static method Main(): int "
        "    var s: long := 1; "
        "    C.Store(ref s); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "does not match the parameter's referent type"));
}

// §22.3 (#1756): the `out` modifier aliases the caller's location just as `ref`
// does, so the same exact-type rule applies to a pre-declared `out` argument.
// Pre-fix this compiled clean.
BEZEL_TEST(ref_out_arg_type_match_1756, out_mismatch_rejected_22_3) {
    auto r = runAot(
        "module M; class C "
        "  public static method Fill(out x: int) x := 7; end Fill; "
        "  public static method Main(): int "
        "    var s: long := 1; "
        "    C.Fill(out s); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "does not match the parameter's referent type"));
}

// =============================================================================
// POSITIVE — a ref/out argument whose type EXACTLY matches the parameter's
// referent type compiles and runs. This is the over-rejection guard: prior
// #1756 reworks over-rejected generic ref/out, so the exact-match non-generic
// control must stay green.
// =============================================================================

// §22.3 (#1756): a `ref n` of type `int` against a `ref x: int` parameter is an
// exact match — compiles, runs, and mutates the caller's storage as expected.
BEZEL_TEST(ref_out_arg_type_match_1756, ref_exact_match_accepted_22_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inc(ref x: int) x := x + 1; end Inc; "
        "  public static method Main(): int "
        "    var n := 5; C.Inc(ref n); return n; "
        "  end Main; "
        "end C;"), 6);
}

// §14.5 / §14.2 (v0.236, #2280): exact generic ref/out unification applies
// when a method type parameter is nested in an inline-array element type. With
// explicit T=int, `[4]long` must not alias the `[4]int` parameter location.
// Pre-fix containsMethodTypeParam skipped arrayElement, so this compiled clean.
BEZEL_TEST(ref_out_arg_type_match_1756,
           inline_array_nested_method_type_param_exactness_unsoundness_rejected_2280_14_5) {
    auto r = runAot(
        "module M; static class U "
        "  public static method Fill<T>(ref r: [4]T) "
        "    r[0] := default(T); "
        "  end Fill; "
        "end U; class C "
        "  public static method Main(): int "
        "    var arr: [4]long; U.Fill<int>(ref arr); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "does not unify exactly"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§14.5"));
}
