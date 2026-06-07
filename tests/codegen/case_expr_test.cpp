// Codegen slice CG28 — `case` expression / pattern matching (§19). The
// scrutinee is evaluated once; arms are tried in order; the first whose
// pattern (and optional `when` guard) matches yields the result. A
// non-matching value raises MatchFailureException (§19.6). This slice
// handles constant, relational-operator, and discard patterns.
// Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg28, constant_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := case x 1 => 10, 2 => 20, 3 => 30 end; return r;"), 20);
}

BEZEL_TEST(codegen_cg28, first_arm_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := case x 1 => 10, 2 => 20 end; return r;"), 10);
}

BEZEL_TEST(codegen_cg28, constant_alternatives) {
    // One arm listing several constants matches any of them.
    BEZEL_EXPECT_EQ(run(
        "var x := 3; "
        "var r := case x 1, 2, 3 => 99, 4 => 0 end; return r;"), 99);
}

BEZEL_TEST(codegen_cg28, discard_is_catch_all) {
    BEZEL_EXPECT_EQ(run(
        "var x := 42; "
        "var r := case x 1 => 1, _ => 7 end; return r;"), 7);
}

// NOTE: relational-operator patterns (`< EXPR`, `>= EXPR`) and range /
// type / binding / null / destructure patterns are deferred — only
// constant, constant-alternative, and discard patterns (plus `when`
// guards) lower in this slice.

BEZEL_TEST(codegen_cg28, guard) {
    // A `when` guard refining a constant pattern (guard uses an outer var).
    BEZEL_EXPECT_EQ(run(
        "var x := 2; var y := 1; "
        "var r := case x "
        "  2 when y > 0 => 50, "
        "  2 => 60, "
        "  _ => 0 "
        "end; return r;"), 50);
}

BEZEL_TEST(codegen_cg28, no_match_raises_match_failure) {
    // No arm matches and there is no discard → MatchFailureException.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "try var r := case x 1 => 10, 2 => 20 end; return r; "
        "catch MatchFailureException => return 77; end;"), 77);
}

// §19.3: range patterns in case arms — `lo..hi` and `lo..<hi`.
BEZEL_TEST(codegen_cg28, range_pattern_hit) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_low_arm) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 1);
}

BEZEL_TEST(codegen_cg28, range_pattern_wildcard_fallthrough) {
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := case x 1..3 => 1, 4..6 => 2, _ => 3 end; return r;"), 3);
}

BEZEL_TEST(codegen_cg28, range_pattern_half_open) {
    // `..<` excludes the upper bound.
    BEZEL_EXPECT_EQ(run(
        "var x := 6; "
        "var r := case x 1..<6 => 1, 6..10 => 2, _ => 0 end; return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_combined_with_constant) {
    // A range pattern and a constant pattern in the same case.
    BEZEL_EXPECT_EQ(run(
        "var x := 0; "
        "var r := case x 0 => 10, 1..5 => 20, _ => 30 end; return r;"), 10);
}

// CG28b / CG38b: binding patterns (`var n`) in case expressions / statements.
// §19.2: the bound name is in scope for the arm guard and body; the binding
// is a borrow of the scrutinee value (same type, always matches).

BEZEL_TEST(codegen_cg28b, binding_pattern_basic) {
    // `var n` always matches; `n` should equal the scrutinee (5).
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := case x var n => n end; return r;"), 5);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_arithmetic) {
    // Body can compute with the bound name.
    BEZEL_EXPECT_EQ(run(
        "var x := 7; "
        "var r := case x var n => n + 1 end; return r;"), 8);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_after_non_match) {
    // A binding pattern following unmatched constant arms catches the value.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := case x 1 => 0, 2 => 0, var n => n * 2 end; return r;"), 18);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_true) {
    // A `when` guard can reference the bound name; guard passes.
    BEZEL_EXPECT_EQ(run(
        "var x := 4; "
        "var r := case x var n when n > 2 => n + 10, _ => 0 end; return r;"),
        14);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_false) {
    // Guard fails → fall through to wildcard.
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := case x var n when n > 2 => n + 10, _ => 99 end; return r;"),
        99);
}

// §19 / §9.4: DestructuringPattern in case expressions — a `TypeName(x, y)`
// pattern on a record value type extracts the positional fields into the arm
// body's scope. The type test is statically satisfied (the scrutinee's static
// type already determines which record it is); field values are extracted via
// llvm::ExtractValue on the struct value.

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_sum_fields) {
    // §19 / §9.4: Point(x, y) extracts both fields; body computes x + y.
    // Expected: 3 + 4 = 7.
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return case p "
        "    Point(x, y) => x + y, "
        "    _ => 0 "
        "  end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_first_field) {
    // §19 / §9.4: destructuring exposes each field independently; use only x.
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return case p "
        "    Point(x, y) => x, "
        "    _ => 0 "
        "  end; "
        "end Main; end C;"), 10);
}

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_second_field) {
    // §19 / §9.4: use only the second bound name (y).
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return case p "
        "    Point(x, y) => y, "
        "    _ => 0 "
        "  end; "
        "end Main; end C;"), 20);
}

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_fallthrough_discard) {
    // §19 / §9.4: a _ arm after a destructuring arm is reachable only when
    // no prior arm matched. Here Point(x, y) always matches a Point scrutinee,
    // so the _ arm is dead; confirm the destructuring arm fires.
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(5, 6); "
        "  return case p "
        "    Point(x, y) => x * y, "
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), 30);
}

// ── DestructuringPattern with mixed constant/binding sub-patterns ─────────
// Previously, `Point(0, y)` was mis-parsed: the ConstantPattern alternatives
// loop consumed `,y` treating `y` as a second constant alternative, leaving
// only one sub-pattern. Fixed by suppressing `,` alternatives inside
// positional-pattern context.

BEZEL_TEST(codegen_cg28_destructure, const_then_binding) {
    // Point(0, y) — first sub-pattern is constant (must match 0),
    // second is binding. Only matches when first field == 0.
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(0, 7); "
        "  return case p "
        "    Pair(0, b) => b, "
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg28_destructure, const_then_binding_no_match) {
    // Point(0, y) should NOT match when first field != 0.
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(5, 7); "
        "  return case p "
        "    Pair(0, b) => b, "
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), -1);
}

BEZEL_TEST(codegen_cg28_destructure, binding_then_const) {
    // Point(x, 0) — matches when second field == 0.
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 0); "
        "  return case p "
        "    Pair(a, 0) => a, "
        "    _ => -1 "
        "  end; "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_cg28_destructure, both_const_no_match) {
    // Point(0, 0) should not match Point(3, 4).
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 4); "
        "  return case p "
        "    Pair(0, 0) => 99, "
        "    _ => 0 "
        "  end; "
        "end Main; end C;"), 0);
}

// ── §19 + §20 + §17.12: `case` EXPRESSION with a diverging arm ───────────
// A `throw` arm (§17.12: `throw` is an expression, valid in any expression
// position) is bottom-typed: it imposes no §20 common-type constraint, and
// at codegen it terminates its block via the throw's unwind so it must be
// excluded from the result PHI. This is the case-expression analog of the
// conditional `x if c else throw E` fix (conditional_test.cpp). Before the
// fix, the result PHI mixed the value arm's i32 with the throw arm's poison
// ptr and tripped LLVM's "all operands to PHI must be the same type"
// assertion. The non-diverging arm is also coerced to the §20 common type
// before the PHI (the numeric-widening tests below).

// Value path: the value arm is selected; the throw arm never executes.
BEZEL_TEST(codegen_cg28, throw_arm_value_path) {
    BEZEL_EXPECT_EQ(run(
        "var c := true; "
        "var x := case c true => 7, _ => throw new Exception(\"e\") end; "
        "return x;"), 7);
}

// Throw path: the discard arm's `throw` fires; an enclosing try/catch in
// the caller catches it. Exercises the diverging-arm lowering at run time.
BEZEL_TEST(codegen_cg28, throw_arm_throw_path) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Helper(c: bool): int "
        "    return case c true => 7, _ => throw new Exception(\"e\") end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    try return Helper(false); "
        "    catch Exception => return 99; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 99);
}

// A `throw` arm appearing FIRST (before a value arm). The value arm is
// reached on the non-matching scrutinee; the throw arm is excluded from the
// PHI just the same regardless of arm order.
BEZEL_TEST(codegen_cg28, throw_arm_first_value_path) {
    BEZEL_EXPECT_EQ(run(
        "var c := false; "
        "var x := case c true => throw new Exception(\"e\"), _ => 8 end; "
        "return x;"), 8);
}

// NOTE on `return` arms: §19.2 admits a single-STATEMENT arm body
// (`=> return E;`) only in STATEMENT-position `case`, and that statement-form
// arm body is not yet parsed (Parser_Exprs.cpp parseCaseArm produces only
// Expression-bodied arms; the statement-form / `do…end` arm body lands with
// the statement-position-case slice). So a `return`-arm case is not yet
// constructible to test. The codegen fix here keys on the same `diverged_`
// flag the conditional fix uses, so it will exclude a `return` arm from the
// result PHI for free once statement-form arms parse — but no test can
// exercise that path until then. The `throw` arm above is the reproducible
// diverging-arm case (throw IS an expression, §17.12) and is covered.

// EVERY arm diverges (all `throw`): the whole case diverges. It must
// compile (no orphaned/ill-typed PHI) and the selected arm's throw fires,
// caught by the caller's try/catch.
BEZEL_TEST(codegen_cg28, all_arms_throw_diverges) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Helper(c: bool): int "
        "    return case c "
        "      true => throw new Exception(\"a\"), "
        "      _ => throw new Exception(\"b\") "
        "    end; "
        "  end Helper; "
        "  public static method Main(): int "
        "    try return Helper(true); "
        "    catch Exception => return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 42);
}

// §20: arms of differing-but-common numeric type. `5` is an int arm and
// `9000000000L` is a long arm; the §20 common type is `long`. Codegen must
// coerce the int arm to i64 in its own block before the result PHI (else an
// i32/i64 PHI mismatch crashes LLVM — the same defect the throw arm hit).
// Verify the long value survives (an i32 PHI would truncate the high bits).
BEZEL_TEST(codegen_cg28, numeric_widening_long_arm) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := false; "
        "    var hi: long := case c true => 5, _ => 9000000000L end; "
        "    return 1 if hi = 9000000000L else 0; "
        "  end Main; "
        "end C;"), 1);
}

// The selected-int direction of the same widened case still yields the int
// value (widened to long, not truncated).
BEZEL_TEST(codegen_cg28, numeric_widening_int_arm) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    var c := true; "
        "    var v: long := case c true => 5, _ => 9000000000L end; "
        "    return 1 if v = 5L else 0; "
        "  end Main; "
        "end C;"), 1);
}

// §20: arms with no common type are rejected with a clean diagnostic, not a
// codegen crash. Here one value arm is `int` and the other is a class value
// `N` — neither is implicitly convertible to the other. (The bottom-typed
// throw arm imposes no constraint, so it is the two value arms that conflict.)
BEZEL_TEST(codegen_cg28, incompatible_value_arms_is_type_error) {
    JitResult r = runJit(
        "module M; class N end N; class C "
        "  public static method Main(): int "
        "    var x := new N(); "
        "    var y := case 1 "
        "      1 => 5, "
        "      _ => x "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// CONTROL: a normal value-only case (all arms the same type) still works
// after the fix — no diverging arm, no widening, a plain int result PHI.
BEZEL_TEST(codegen_cg28, control_all_value_arms_same_type) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := case x 1 => 10, 2 => 20, _ => 30 end; return r;"), 20);
}

// ── §19 / §7 / issues #447 + #448: case over nullable VALUE types ─────────────
// A nullable value type lowers to {i8 hasValue; T value}. The existing case
// lowering passes the struct scrutinee to patternMatches, which previously tried
// to emit pointer comparisons (null pattern) or scalar comparisons with the full
// struct (constant/enum patterns) — both generate illegal IR / SIGILL (#447).
// The fix checks the scrutinee's TypeRef: when it is a nullable value type, null
// arms test !hasValue, constant arms test hasValue AND value==const, and _ is
// unchanged (always true).

// #447: case over int? — value arm (42), null arm, _ arm (no crash, correct sel).
BEZEL_TEST(codegen_cg28_nullable_value_case, int_nullable_value_arm_spec_19_7_issue_447) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := 42; "
        "var r := case x "
        "  42 => 1, "
        "  null => 2, "
        "  _ => 3 "
        "end; return r;"), 1);
}

// #447: null arm selected when x: int? = null
BEZEL_TEST(codegen_cg28_nullable_value_case, int_nullable_null_arm_spec_19_7_issue_447) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := null; "
        "var r := case x "
        "  42 => 1, "
        "  null => 2, "
        "  _ => 3 "
        "end; return r;"), 2);
}

// #447: _ arm fires when no constant or null arm matches
BEZEL_TEST(codegen_cg28_nullable_value_case, int_nullable_discard_arm_spec_19_7_issue_447) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := 7; "
        "var r := case x "
        "  42 => 1, "
        "  _ => 99 "
        "end; return r;"), 99);
}

// #447: nullable enum case — value arm selected (Color.Green)
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_value_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Green; "
        "  return case c "
        "    Color.Green => 10, "
        "    Color.Red => 20, "
        "    null => 30, "
        "    _ => 40 "
        "  end; "
        "end Main; end C;"), 10);
}

// #447: nullable enum case — null arm selected when c: Color? = null
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_null_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := null; "
        "  return case c "
        "    Color.Green => 10, "
        "    null => 30, "
        "    _ => 40 "
        "  end; "
        "end Main; end C;"), 30);
}

// #447: nullable enum — _ (discard) arm fires when no value arm matches
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_discard_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Blue; "
        "  return case c "
        "    Color.Green => 10, "
        "    null => 30, "
        "    _ => 99 "
        "  end; "
        "end Main; end C;"), 99);
}

// REGRESSION: non-nullable int case still works after the fix
BEZEL_TEST(codegen_cg28_nullable_value_case, nonnull_int_case_regression) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := case x 5 => 1, _ => 0 end; return r;"), 1);
}

// REGRESSION: non-nullable enum case still works after the fix
BEZEL_TEST(codegen_cg28_nullable_value_case, nonnull_enum_case_regression) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color.Red; "
        "  return case c "
        "    Color.Green => 10, "
        "    Color.Red => 20, "
        "    _ => 30 "
        "  end; "
        "end Main; end C;"), 20);
}
