// Spec-derived regression test for issue #5401 (spec §19.3, v0.314).
//
// THE DEFECT. When a case arm's pattern binds multiple names, bindings of ONE
// pattern (whether bare identifiers or explicit `var` bindings) must be distinct
// per §19.3. Three sites in ExprTypecheck_Exprs.cpp:
//   - nakedBinding in record-case (line ~13320)
//   - BindingPattern name in record-case (line ~13339)
//   - nakedBinding in lambda destructure (line ~727)
// had no distinctness check.
//
// (The two analogous sites for a choice-type scrutinee could not be tested:
// choice types do not support positional destructuring patterns per §19.3;
// they require explicit case-name syntax (`Some(x)`) which binds each arm's
// payload independently, so a single arm can never bind the same name twice.)
//
// THE FIX. Each site now checks `armScope.names` / `scope.names` before
// registering a binding and emits: "a local named `X` is already declared in
// this scope (§19.3, §21.1, v0.314)".
//
// OUT OF SCOPE: setter's implicit `value` param collision (different issue).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// -----------------------------------------------------------------------------
// §19.3 Sites (record-case scrutinee): the 3 distinct code paths
// -----------------------------------------------------------------------------

// Site A: nakedBinding in record-case (line ~13320)
// `case p of Pt(x, x) =>` — same bare identifier twice in one pattern.
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           case_record_naked_duplicate_rejected) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(x, x) => x);"
        "  end M;"
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `x` is already declared in this scope (§19.3"));
}

// Site B: BindingPattern name in record-case (line ~13339)
// `case p of Pt(var x, var x) =>` — same explicit var binding twice.
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           case_record_explicit_var_duplicate_rejected) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(var x, var x) => x);"
        "  end M;"
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `x` is already declared in this scope (§19.3"));
}

// Site C: nakedBinding in lambda destructure (line ~727)
// `lambda(Entry(x, x)) =>` — lambda parameter binds same name twice.
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           lambda_destructure_naked_duplicate_rejected) {
    auto r = runAot(
        "module M;"
        "record Entry(first: int, second: int);"
        "class C"
        "  public static method Main(): int"
        "    var f: (Entry) -> int := lambda(Entry(x, x)) => x;"
        "    return 0;"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `x` is already declared in this scope (§19.3"));
}

// -----------------------------------------------------------------------------
// Controls: distinct names in patterns must still compile
// -----------------------------------------------------------------------------

// Control: distinct bare identifiers in record pattern — accepted
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           control_case_record_naked_distinct_accepted) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(x, y) => x + y, _ => 0);"
        "  end M;"
        "  public static method Main(): int return M(new Pt(1, 2)); end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// Control: distinct var bindings in record pattern — accepted
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           control_case_record_var_distinct_accepted) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(var x, var y) => x + y, _ => 0);"
        "  end M;"
        "  public static method Main(): int return M(new Pt(1, 2)); end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// Control: distinct names in lambda destructure — accepted
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           control_lambda_destructure_distinct_accepted) {
    auto r = runAot(
        "module M;"
        "record Entry(first: int, second: int);"
        "class C"
        "  public static method Main(): int"
        "    var f: (Entry) -> int := lambda(Entry(x, y)) => x + y;"
        "    return f(new Entry(1, 2));"
        "  end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// Control: discard `_` is exempt and may appear multiple times
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           control_discards_multiple_accepted) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(_, _) => 99, _ => 0);"
        "  end M;"
        "  public static method Main(): int return M(new Pt(1, 2)); end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// Control: same name in DIFFERENT case arms is fine (each arm has its own scope)
BEZEL_TEST(pattern_binding_distinctness_reject_5401,
           control_same_name_different_arms_accepted) {
    auto r = runAot(
        "module M;"
        "record Pt(a: int, b: int);"
        "class C"
        "  public static method M(p: Pt): int"
        "    return (case p of Pt(x, _) => x, Pt(_, y) => y);"
        "  end M;"
        "  public static method Main(): int return M(new Pt(3, 4)); end Main;"
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
