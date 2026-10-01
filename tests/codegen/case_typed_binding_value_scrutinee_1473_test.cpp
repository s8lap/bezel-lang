// Codegen — fix #1473 / §19.3 / §19.5 / §3.3: a typed binding pattern
// `var b: P` over a VALUE-type scrutinee is a type-test — it may match only
// when the scrutinee's runtime type IS P. Two mismatch shapes were unsound:
//
//   (a) CONCRETE value/value mismatch — an `int` scrutinee vs `var b: bool`.
//       codegen's primitive-binding branch (patternMatches) fell through to an
//       UNCONDITIONAL `return getInt1(true)`, so the arm matched regardless of
//       type and the scrutinee's raw bits were bound under the wrong type: the
//       wrong arm was selected (wrong runtime value — the repro printed "BOOL"
//       for an `int` scrutinee).
//   (b) GENERIC per-instantiation (§3.3) — a `T`-typed scrutinee (T
//       monomorphized to a value type) vs `var b: int`. The scrutinee's
//       exprType is still the placeholder param ref, so the same-primitive test
//       computed false and every instantiation matched unconditionally.
//
// Fix (LlvmBackend_Exprs2.cpp patternMatches, Kind::Primitive binding branch):
// resolve the scrutinee's type-param ref to its concrete substituted type, then
// gate the final match on `scrutIsSamePrim` instead of returning unconditional
// true. This mirrors the BARE type-pattern form (`bool` over an `int` scrutinee
// already yields no match) and §19.6's constant-never-match discipline: a
// mismatched typed binding simply falls through to the next / `_` arm.
//
// The pre-existing §19.5 sema reject for a value-scrutinee vs a REFERENCE-typed
// binding (`var s: string` / `var d: Dog` over `int`) is unaffected and still
// fires — those codegen paths (emitIsA / string-GEP) assume a pointer and would
// abort on an i32, so they remain a clean compile error.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
// A compile-time error was reported (the §19.5 reference-binding reject).
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}
}  // namespace

// NOTE (v0.313, #5188): the two concrete value/value mismatch rows below
// (bool, double over int) now pin the compile-time reject, not the runtime
// `_` arm; the original #1473 runtime behavior is superseded by §19.3.
// §19.3/§19.5 (#1473): the issue's concrete repro — an `int` scrutinee vs a
// `var b: bool` binding can NEVER match, so the `_` arm is selected (0), not the
// mismatched binding arm (1). Pre-fix this returned 1 (unsound).
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, int_scrut_bool_binding_rejected_5188) {
    // §19.3 / Appendix C.1 (v0.313, #5188) FLIP (was: never matches, `_` arm,
    // exit 0): an `int` scrutinee vs `var b: bool` can never succeed and is now
    // a compile error.
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return (case x of var b: bool => 1, _ => 0); "
        "end Main; end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "naming a different value type"));
}

// §19.3 (#1473): a `double`-typed binding over an `int` scrutinee likewise never
// matches — falls through to `_`.
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, int_scrut_double_binding_rejected_5188) {
    // §19.3 / Appendix C.1 (v0.313, #5188) FLIP (was: never matches, exit 0).
    auto r = runAot(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return (case x of var d: double => 1, _ => 0); "
        "end Main; end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "naming a different value type"));
}

// Regression (#1473): the statically-satisfied case still matches — a `var n:
// int` binding over an `int` scrutinee matches and binds the value (the guard
// observes n = 5), returning 42.
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, int_scrut_int_binding_matches_and_binds) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return (case x of var n: int when n = 5 => 42, _ => 1); "
        "end Main; end App;"), 42);
}

// Regression (#1473): a matching `var b: bool` over a `bool` scrutinee still
// matches and binds (guard observes b), returning 7.
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, bool_scrut_bool_binding_matches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class App public static method Main(): int "
        "  var x: bool := true; "
        "  return (case x of var b: bool when b => 7, _ => 1); "
        "end Main; end App;"), 7);
}

// §3.3 per-instantiation (#1473): a `var b: int` binding over a generic
// `T`-typed scrutinee matches ONLY the instantiation whose runtime type is int.
// Kind<int>.Code() -> 1 (matches), Kind<char>.Code() -> 0 (no match).
// Encoded as 1*10 + 0 = 10. Pre-fix BOTH matched -> 1*10 + 1 = 11.
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, generic_scrut_matches_only_int_instantiation) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Kind<T> "
        "  private v: T; "
        "  public constructor(x: T) v := x; end; "
        "  public method Code(): int => (case v of var b: int => 1, _ => 0); "
        "end Kind; "
        "public static class P public static method Main(): int "
        "  return new Kind<int>(5).Code() * 10 + new Kind<char>('z').Code(); "
        "end Main; end P;"), 10);
}

// §19.5 (#1473, guard against regressing the sibling reject): a value scrutinee
// vs a REFERENCE-typed binding (`var s: string`) is still a clean compile error,
// not an unconditional match or a backend abort.
BEZEL_TEST(case_typed_binding_value_scrutinee_1473, int_scrut_string_binding_still_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; class App public static method Main(): int "
        "  var x: int := 5; "
        "  return (case x of var s: string => 1, _ => 0); "
        "end Main; end App;"), true);
}
