// Codegen slice CG28 — `case` expression / pattern matching (§19). The
// scrutinee is evaluated once; arms are tried in order; the first whose
// pattern (and optional `when` guard) matches yields the result. A
// non-matching value raises MatchFailureException (§19.6). This slice
// handles constant, relational-operator, and discard patterns.
// Observable via the int exit code.

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg28, constant_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := (case x of 1 => 10, 2 => 20, 3 => 30); return r;"), 20);
}

BEZEL_TEST(codegen_cg28, first_arm_match) {
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := (case x of 1 => 10, 2 => 20); return r;"), 10);
}

BEZEL_TEST(codegen_cg28, constant_alternatives) {
    // One arm listing several constants matches any of them.
    BEZEL_EXPECT_EQ(run(
        "var x := 3; "
        "var r := (case x of 1 | 2 | 3 => 99, 4 => 0); return r;"), 99);
}

BEZEL_TEST(codegen_cg28, discard_is_catch_all) {
    BEZEL_EXPECT_EQ(run(
        "var x := 42; "
        "var r := (case x of 1 => 1, _ => 7); return r;"), 7);
}

// NOTE: relational-operator patterns (`< EXPR`, `>= EXPR`) and range /
// type / binding / null / destructure patterns are deferred — only
// constant, constant-alternative, and discard patterns (plus `when`
// guards) lower in this slice.

BEZEL_TEST(codegen_cg28, guard) {
    // A `when` guard refining a constant pattern (guard uses an outer var).
    BEZEL_EXPECT_EQ(run(
        "var x := 2; var y := 1; "
        "var r := (case x of "
        "  2 when y > 0 => 50, "
        "  2 => 60, "
        "  _ => 0); return r;"), 50);
}

BEZEL_TEST(codegen_cg28, no_match_raises_match_failure) {
    // No arm matches and there is no discard → MatchFailureException.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "try var r := (case x of 1 => 10, 2 => 20); return r; "
        "catch MatchFailureException => return 77; end;"), 77);
}

// §19.3: range patterns in case arms — `lo..hi` and `lo..<hi`.
BEZEL_TEST(codegen_cg28, range_pattern_hit) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := (case x of 1..3 => 1, 4..6 => 2, _ => 3); return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_low_arm) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := (case x of 1..3 => 1, 4..6 => 2, _ => 3); return r;"), 1);
}

BEZEL_TEST(codegen_cg28, range_pattern_wildcard_fallthrough) {
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := (case x of 1..3 => 1, 4..6 => 2, _ => 3); return r;"), 3);
}

BEZEL_TEST(codegen_cg28, range_pattern_half_open) {
    // `..<` excludes the upper bound.
    BEZEL_EXPECT_EQ(run(
        "var x := 6; "
        "var r := (case x of 1..<6 => 1, 6..10 => 2, _ => 0); return r;"), 2);
}

BEZEL_TEST(codegen_cg28, range_pattern_combined_with_constant) {
    // A range pattern and a constant pattern in the same case.
    BEZEL_EXPECT_EQ(run(
        "var x := 0; "
        "var r := (case x of 0 => 10, 1..5 => 20, _ => 30); return r;"), 10);
}

// CG28b / CG38b: binding patterns (`var n`) in case expressions / statements.
// §19.2: the bound name is in scope for the arm guard and body; the binding
// is a borrow of the scrutinee value (same type, always matches).

BEZEL_TEST(codegen_cg28b, binding_pattern_basic) {
    // `var n` always matches; `n` should equal the scrutinee (5).
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := (case x of var n => n); return r;"), 5);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_arithmetic) {
    // Body can compute with the bound name.
    BEZEL_EXPECT_EQ(run(
        "var x := 7; "
        "var r := (case x of var n => n + 1); return r;"), 8);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_after_non_match) {
    // A binding pattern following unmatched constant arms catches the value.
    BEZEL_EXPECT_EQ(run(
        "var x := 9; "
        "var r := (case x of 1 => 0, 2 => 0, var n => n * 2); return r;"), 18);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_true) {
    // A `when` guard can reference the bound name; guard passes.
    BEZEL_EXPECT_EQ(run(
        "var x := 4; "
        "var r := (case x of var n when n > 2 => n + 10, _ => 0); return r;"),
        14);
}

BEZEL_TEST(codegen_cg28b, binding_pattern_with_guard_false) {
    // Guard fails → fall through to wildcard.
    BEZEL_EXPECT_EQ(run(
        "var x := 1; "
        "var r := (case x of var n when n > 2 => n + 10, _ => 99); return r;"),
        99);
}

// §19 / §9.4: DestructuringPattern in case expressions — a `TypeName(x, y)`
// pattern on a record value type extracts the positional fields into the arm
// body's scope. The type test is statically satisfied (the scrutinee's static
// type already determines which record it is); field values are extracted via
// llvm::ExtractValue on the struct value.

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
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
        "  return (case p of "
        "    Point(x, y) => x + y, "
        "    _ => 0); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_first_field) {
    // §19 / §9.4: destructuring exposes each field independently; use only x.
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return (case p of "
        "    Point(x, y) => x, "
        "    _ => 0); "
        "end Main; end C;"), 10);
}

BEZEL_TEST(codegen_cg28_destructure, record_case_expr_second_field) {
    // §19 / §9.4: use only the second bound name (y).
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return (case p of "
        "    Point(x, y) => y, "
        "    _ => 0); "
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
        "  return (case p of "
        "    Point(x, y) => x * y, "
        "    _ => -1); "
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
        "  return (case p of "
        "    Pair(0, b) => b, "
        "    _ => -1); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg28_destructure, const_then_binding_no_match) {
    // Point(0, y) should NOT match when first field != 0.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(5, 7); "
        "  Console.WriteLine((case p of "
        "    Pair(0, b) => b, "
        "    _ => -1)); return 0; "
        "end Main; end C;"), "-1");
}

BEZEL_TEST(codegen_cg28_destructure, binding_then_const) {
    // Point(x, 0) — matches when second field == 0.
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 0); "
        "  return (case p of "
        "    Pair(a, 0) => a, "
        "    _ => -1); "
        "end Main; end C;"), 3);
}

BEZEL_TEST(codegen_cg28_destructure, both_const_no_match) {
    // Point(0, 0) should not match Point(3, 4).
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pair(a: int, b: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 4); "
        "  return (case p of "
        "    Pair(0, 0) => 99, "
        "    _ => 0); "
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
        "var x := (case c of true => 7, _ => throw new Exception(\"e\")); "
        "return x;"), 7);
}

// Throw path: the discard arm's `throw` fires; an enclosing try/catch in
// the caller catches it. Exercises the diverging-arm lowering at run time.
BEZEL_TEST(codegen_cg28, throw_arm_throw_path) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Helper(c: bool): int "
        "    return (case c of true => 7, _ => throw new Exception(\"e\")); "
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
        "var x := (case c of true => throw new Exception(\"e\"), _ => 8); "
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
        "    return (case c of "
        "      true => throw new Exception(\"a\"), "
        "      _ => throw new Exception(\"b\")); "
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
        "    var hi: long := (case c of true => 5, _ => 9000000000L); "
        "    return (if hi = 9000000000L then 1 else 0); "
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
        "    var v: long := (case c of true => 5, _ => 9000000000L); "
        "    return (if v = 5L then 1 else 0); "
        "  end Main; "
        "end C;"), 1);
}

// §20: arms with no common type are rejected with a clean diagnostic, not a
// codegen crash. Here one value arm is `int` and the other is a class value
// `N` — neither is implicitly convertible to the other. (The bottom-typed
// throw arm imposes no constraint, so it is the two value arms that conflict.)
BEZEL_TEST(codegen_cg28, incompatible_value_arms_is_type_error) {
    JitResult r = runAot(
        "module M; class N end N; class C "
        "  public static method Main(): int "
        "    var x := new N(); "
        "    var y := (case 1 of "
        "      1 => 5, "
        "      _ => x); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // fix #3895 (issue #3892): a bare `hasErrors()`/`!ran` pair cannot tell a
    // genuine type-error rejection apart from a runtime crash/signal.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL: a normal value-only case (all arms the same type) still works
// after the fix — no diverging arm, no widening, a plain int result PHI.
BEZEL_TEST(codegen_cg28, control_all_value_arms_same_type) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := (case x of 1 => 10, 2 => 20, _ => 30); return r;"), 20);
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
        "var r := (case x of "
        "  42 => 1, "
        "  null => 2, "
        "  _ => 3); return r;"), 1);
}

// #447: null arm selected when x: int? = null
BEZEL_TEST(codegen_cg28_nullable_value_case, int_nullable_null_arm_spec_19_7_issue_447) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := null; "
        "var r := (case x of "
        "  42 => 1, "
        "  null => 2, "
        "  _ => 3); return r;"), 2);
}

// #447: _ arm fires when no constant or null arm matches
BEZEL_TEST(codegen_cg28_nullable_value_case, int_nullable_discard_arm_spec_19_7_issue_447) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := 7; "
        "var r := (case x of "
        "  42 => 1, "
        "  _ => 99); return r;"), 99);
}

// #447: nullable enum case — value arm selected (Color.Green)
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_value_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Green; "
        "  return (case c of "
        "    Color.Green => 10, "
        "    Color.Red => 20, "
        "    null => 30, "
        "    _ => 40); "
        "end Main; end C;"), 10);
}

// #447: nullable enum case — null arm selected when c: Color? = null
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_null_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := null; "
        "  return (case c of "
        "    Color.Green => 10, "
        "    null => 30, "
        "    _ => 40); "
        "end Main; end C;"), 30);
}

// #447: nullable enum — _ (discard) arm fires when no value arm matches
BEZEL_TEST(codegen_cg28_nullable_value_case, enum_nullable_discard_arm_spec_19_8_issue_447) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := Color.Blue; "
        "  return (case c of "
        "    Color.Green => 10, "
        "    null => 30, "
        "    _ => 99); "
        "end Main; end C;"), 99);
}

// REGRESSION: non-nullable int case still works after the fix
BEZEL_TEST(codegen_cg28_nullable_value_case, nonnull_int_case_regression) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := (case x of 5 => 1, _ => 0); return r;"), 1);
}

// REGRESSION: non-nullable enum case still works after the fix
BEZEL_TEST(codegen_cg28_nullable_value_case, nonnull_enum_case_regression) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "enum Color Red := 0, Green := 1, Blue := 2; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color.Red; "
        "  return (case c of "
        "    Color.Green => 10, "
        "    Color.Red => 20, "
        "    _ => 30); "
        "end Main; end C;"), 20);
}

// ── #473 / §19.3 + §19.5: value-producing `case` EXPRESSION with type-test
// and typed-binding pattern arms ─────────────────────────────────────────────
// A bare type name `Dog` parses as a NamedPattern; a typed binding `var x: T`
// is a BindingPattern with a type annotation. Both are TYPE-TEST patterns
// (§19.3: "Subtypes match: a Dog value matches an Animal pattern"; §19.5: a
// type pattern matches only non-null values). Before the fix, the codegen
// `patternMatches` returned null for these on a class scrutinee, so
// `lowerCaseExpr` bailed and left `case.end` (or an intermediate block) with
// no terminator — the always-on IR verifier rejected it (and earlier could
// crash bezelc with SIGILL/assert). Each test asserts the RETURNED value, so
// it proves the result PHI / merge is correct, not merely that it compiles.

// #473 B1: a bare type-pattern arm (`Dog => 1, _ => 0`) selects the type arm
// when the (statically Animal) scrutinee is a Dog at run time. Returns 1.
BEZEL_TEST(codegen_cg28_473, type_pattern_arm_hit_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  return (case a of "
        "    Dog => 1, "
        "    _ => 0); "
        "end Main; end C;"), 1);
}

// #473 B1 no-match path: a Cat scrutinee does NOT match `Dog =>`, so the `_`
// arm yields 0. Confirms emitIsA's type test is exact (subtype, not any-ref).
BEZEL_TEST(codegen_cg28_473, type_pattern_arm_no_match_falls_to_discard) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class C public static method Main(): int "
        "  using var c := new Cat(); "
        "  var a: Animal := c; "
        "  return (case a of "
        "    Dog => 1, "
        "    _ => 0); "
        "end Main; end C;"), 0);
}

// #473 B1 subtype matching (§19.3): a `Dog` value matches an `Animal` pattern.
BEZEL_TEST(codegen_cg28_473, type_pattern_subtype_matches_base_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  return (case a of "
        "    Animal => 1, "
        "    _ => 0); "
        "end Main; end C;"), 1);
}

// #473 B2: a typed-binding arm (`var x: Dog`) followed by a bare type-pattern
// arm. The binding arm matches and binds x; the body uses x. Returns 1.
BEZEL_TEST(codegen_cg28_473, binding_then_type_pattern_bound_value_used_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal "
        "  public method Tag(): int return 11; end Tag; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  return (case a of "
        "    var x: Dog => x.Tag(), "
        "    Dog => 2, "
        "    _ => 0); "
        "end Main; end C;"), 11);
}

// #473 B2 variant: binding + `when` guard then another arm. The guard FAILS,
// so the arm is skipped and the next (bare `Dog`) arm matches → 2. Exercises
// the guard block + merge from a subsequent arm.
BEZEL_TEST(codegen_cg28_473, binding_with_guard_false_falls_through_spec_19_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal "
        "  public method Size(): int return 3; end Size; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  return (case a of "
        "    var x: Dog when x.Size() > 5 => 1, "
        "    Dog => 2, "
        "    _ => 0); "
        "end Main; end C;"), 2);
}

// #473 B2 variant: binding + `when` guard that PASSES — selects the guarded
// arm and the bound name is usable in the body. Returns 13 (3 + 10).
BEZEL_TEST(codegen_cg28_473, binding_with_guard_true_selects_arm_spec_19_4) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal "
        "  public method Size(): int return 3; end Size; "
        "end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  return (case a of "
        "    var x: Dog when x.Size() < 5 => x.Size() + 10, "
        "    Dog => 2, "
        "    _ => 0); "
        "end Main; end C;"), 13);
}

// #473 B2 variant: null pattern then a typed-binding arm on a NULLABLE
// reference scrutinee. The scrutinee is a non-null Dog → the null arm is
// skipped and the `var x: Dog` arm matches (§19.5: a type pattern matches
// only non-null values, which holds here). Returns 7.
BEZEL_TEST(codegen_cg28_473, null_then_binding_nonnull_ref_spec_19_5) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal? := d; "
        "  return (case a of "
        "    null => 5, "
        "    var x: Dog => 7, "
        "    _ => 9); "
        "end Main; end C;"), 7);
}

// #473 B2 variant: same shape but the scrutinee IS null → the null arm fires
// (§19.5: the type pattern does NOT match null). Returns 5.
BEZEL_TEST(codegen_cg28_473, null_then_binding_null_ref_spec_19_5) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class C public static method Main(): int "
        "  var a: Animal? := null; "
        "  return (case a of "
        "    null => 5, "
        "    var x: Dog => 7, "
        "    _ => 9); "
        "end Main; end C;"), 5);
}

// #473 B2 variant: constant pattern then a typed-binding to a primitive type
// (`0 => .., var v: int => v`). `var v: int` on an int scrutinee is a
// statically-satisfied type test that binds the value (§19.3). The constant
// arm misses (scrutinee 5 != 0), the binding arm catches it → 5.
BEZEL_TEST(codegen_cg28_473, constant_then_primitive_binding_spec_19_3) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 5; "
        "var r := (case a of 0 => 100, var v: int => v); return r;"), 5);
}

// #473 B2 variant: the constant arm HITS (scrutinee 0) — the primitive
// binding arm is not reached. Returns 100.
BEZEL_TEST(codegen_cg28_473, constant_then_primitive_binding_const_hit_spec_19_3) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 0; "
        "var r := (case a of 0 => 100, var v: int => v); return r;"), 100);
}

// #473: a bare primitive type pattern `int` (TypePattern) on an int scrutinee.
// §19.3: a type pattern without binding matches the value; on a non-nullable
// int it always matches. The constant arm misses → the `int` arm yields 42.
BEZEL_TEST(codegen_cg28_473, bare_primitive_type_pattern_spec_19_3) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 9; "
        "var r := (case a of 0 => 1, int => 42, _ => 0); return r;"), 42);
}

// #473 mixed: 3+ arms with a TYPE pattern in the MIDDLE — exercises the merge
// (PHI edge) from a middle arm whose pattern emitted a type test. The middle
// `Cat` arm matches a Cat scrutinee → 2.
BEZEL_TEST(codegen_cg28_473, type_pattern_in_middle_arm_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class Fish : Animal end Fish; "
        "class C public static method Main(): int "
        "  using var c := new Cat(); "
        "  var a: Animal := c; "
        "  return (case a of "
        "    Dog => 1, "
        "    Cat => 2, "
        "    Fish => 3, "
        "    _ => 4); "
        "end Main; end C;"), 2);
}

// #473 REGRESSION: a value-only case (constant + discard arms) still works
// after the fix — a plain int result PHI, no type tests.
BEZEL_TEST(codegen_cg28_473, regression_constant_discard_case_still_works) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "var r := (case x of 1 => 10, 2 => 20, _ => 30); return r;"), 20);
}

// #473 REGRESSION: the case STATEMENT form with a type-test pattern still
// works (the statement form is what the `is`/exhaustiveness paths use). A
// type-test arm dispatches and a side effect records the selected branch.
BEZEL_TEST(codegen_cg28_473, regression_case_statement_type_pattern_still_works) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Animal end Animal; "
        "class Dog : Animal end Dog; "
        "class Cat : Animal end Cat; "
        "class C public static method Main(): int "
        "  using var d := new Dog(); "
        "  var a: Animal := d; "
        "  var r: int := 0; "
        "  case a of "
        "    Cat => do r := 1; end, "
        "    Dog => do r := 2; end, "
        "    _ => do r := 3; end "
        "  end; "
        "  return r; "
        "end Main; end C;"), 2);
}

// #726 / §19.3: a relational case pattern with a NON-CONSTANT (variable)
// operand. Previously the operand wasn't typechecked, so codegen could not
// lower it and `case.end` was left unterminated (IR-verify failure). The case
// visitors now visit the RelOpPattern operand. x=7, > limit(5) → 1.
BEZEL_TEST(codegen_cg28, relational_pattern_variable_operand_726) {
    BEZEL_EXPECT_EQ(run(
        "var limit := 5; var x := 7; "
        "var r := (case x of "
        "    > limit => 1, "
        "    _ => 0); "
        "return r;"), 1);
}

// #726: >= and < relational patterns with variable operands.
BEZEL_TEST(codegen_cg28, relational_pattern_ge_lt_variable_726) {
    BEZEL_EXPECT_EQ(run(
        "var lo := 10; var x := 3; "
        "var r := (case x of "
        "    >= lo => 100, "
        "    < lo => 7, "
        "    _ => 0); "
        "return r;"), 7);  // 3 < 10
}

// #726 / §16.2 / §19.3: a TRAILING-open range pattern (`31..` matches values at
// or above 31). Previously IR-failed — codegen's range-pattern handler required
// both bounds; it now matches an absent upper (or lower) bound.
BEZEL_TEST(codegen_cg28, trailing_open_range_pattern_726) {
    BEZEL_EXPECT_EQ(run(
        "var x := 80; "
        "var r := (case x of "
        "    80.. => 2, "
        "    _ => 0); "
        "return r;"), 2);  // 80 >= 80
}

// #726: the trailing-open lower bound is inclusive (`31..` matches 31).
BEZEL_TEST(codegen_cg28, trailing_open_range_boundary_726) {
    BEZEL_EXPECT_EQ(run(
        "var x := 31; "
        "var r := (case x of "
        "    31.. => 9, "
        "    _ => 0); "
        "return r;"), 9);
}

// #744 / §19.3: a LEADING-open range pattern (`..<0` matches values below 0).
// This was previously rejected at parse time as ambiguous with a
// `scrutinee..hi` range; with the scrutinee-less form now spelled `when`, the
// `case` scrutinee parses above the range level so a leading `..`/`..<` opens
// the arm pattern, not a scrutinee range. The upper bound is EXCLUSIVE.
BEZEL_TEST(codegen_cg28, leading_open_exclusive_range_pattern_744) {
    BEZEL_EXPECT_EQ(run(
        "var x := -3; "
        "var r := (case x of "
        "    ..<0 => 1, "
        "    _ => 0); "
        "return r;"), 1);  // -3 < 0
}

// #744 / §19.3: `..<0` is exclusive of 0 — the value 0 falls through.
BEZEL_TEST(codegen_cg28, leading_open_exclusive_boundary_744) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; "
        "var r := (case x of "
        "    ..<0 => 1, "
        "    _ => 0); "
        "return r;"), 0);  // 0 is not < 0
}

// #744 / §19.3: an INCLUSIVE leading-open range (`..5`) matches up to and
// including 5; the boundary 5 matches.
BEZEL_TEST(codegen_cg28, leading_open_inclusive_boundary_744) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := (case x of "
        "    ..5 => 7, "
        "    _ => 0); "
        "return r;"), 7);  // 5 <= 5
}

// #744 / §19.3: a leading-open pattern as the FIRST arm must NOT be swallowed
// into the scrutinee. `case x ..<10 => 1, 10.. => 2` with x = 3 selects arm 1,
// proving `x` (not `x..<10`) is the scrutinee.
BEZEL_TEST(codegen_cg28, leading_open_first_arm_not_swallowed_744) {
    BEZEL_EXPECT_EQ(run(
        "var x := 3; "
        "var r := (case x of "
        "    ..<10 => 1, "
        "    10.. => 2, "
        "    _ => 0); "
        "return r;"), 1);  // 3 < 10
}

// #738 / §16.4: `step` is not legal in a `case` range pattern (a pattern tests
// membership; a stride has no meaning). Previously this reached codegen and
// emitted malformed IR (unterminated case.end); now it is a clean diagnostic.
BEZEL_TEST(codegen_cg28, step_in_range_pattern_rejected_738) {
    JitResult r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 4; "
        "  var r := (case x of 0..10 step 2 => 1, _ => 0); "
        "  return r; "
        "end Main; end C;");
    // fix #3895 (issue #3892): see incompatible_value_arms_is_type_error above.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// #738 / §16.4: a `step` in a for-in range is NOT a pattern and stays legal.
BEZEL_TEST(codegen_cg28, step_in_for_in_range_still_ok_738) {
    BEZEL_EXPECT_EQ(run(
        "var s := 0; "
        "for var i in 0..10 step 2 do s := s + i; end; "
        "return s;"), 30);  // 0+2+4+6+8+10
}

// #738 / §19.6: an unguarded `_` catch-all must be the last arm; an earlier one
// makes the following arms unreachable — now a clean diagnostic.
BEZEL_TEST(codegen_cg28, discard_not_last_arm_rejected_738) {
    JitResult r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 5; "
        "  var r := (case x of _ => 0, 1 => 1, 5 => 5); "
        "  return r; "
        "end Main; end C;");
    // fix #3895 (issue #3892): see incompatible_value_arms_is_type_error above.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// #738 / §19.6: a GUARDED `_ when g` is conditional, not a catch-all, so it may
// appear before the last arm.
BEZEL_TEST(codegen_cg28, guarded_discard_before_last_ok_738) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "var r := (case x of _ when x > 100 => 0, 5 => 5, _ => 9); "
        "return r;"), 5);
}

// #738 / §19.3+§19.5: a typed binding pattern `var n: int` over an `int?`
// scrutinee. Previously IR-failed (nullable-value path deferred BindingPattern);
// now it matches only the non-null value and binds the UNWRAPPED int.
BEZEL_TEST(codegen_cg28, typed_binding_over_nullable_value_738) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := 5; "
        "var r := (case x of var n: int => n, _ => 99); "
        "return r;"), 5);  // present → bound unwrapped
}
BEZEL_TEST(codegen_cg28, typed_binding_over_nullable_value_null_738) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := null; "
        "var r := (case x of var n: int => n, _ => 99); "
        "return r;"), 99);  // §19.5: typed binding never matches null
}

// §19.3 / §16.6 (v0.286, #3298) — REJECTION, reversing #738's two acceptance
// rows. #738 taught the nullable-value RangePattern lowering ("matches hasValue
// AND in-range"); v0.267 deliberately overrode that reading (proposal 0114's
// declined D5) and §19.3 (v0.310, #5143 ruling) reads: "A nullable scrutinee
// — value or reference — is a compile error at a relational pattern, and at a
// range pattern (§16.6) alike: ordering admits no absent participant (§7.11)"
// for an UN-narrowed scrutinee. An explicit `null` arm beside the range does
// not rescue it — the rejection is on the range pattern itself. The absent
// case is written `case x of null => …, var n: int when n >= 5 and n <= 10
// => …`.
//
// fix #5173 (v0.310): a §7.4-narrowed non-null read of a nullable scrutinee
// is ADMITTED. The narrowed variant is covered by
// `range_pattern_over_narrowed_nullable_value_admitted_5173` below — runAot
// over the lowered program selects the `5..10` arm and prints `1`.
//
// #738's OTHER subject — a typed binding pattern over a nullable scrutinee —
// is unaffected and still covered by the rows above.
BEZEL_TEST(codegen_cg28, range_pattern_over_nullable_value_rejected_3298) {
    auto r = runAot(
        "module M; class C public static method Main(x: int?): int "
        "  var v := (case x of null => 0, 5..10 => 1, _ => 2); "
        "  return v; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("range pattern requires a non-nullable scrutinee") !=
                std::string::npos &&
            d.message.find("19.3") != std::string::npos)
            sawRule = true;
    BEZEL_EXPECT_TRUE(sawRule);
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed nullable scrutinee,
// range pattern. patternMatches's `isNullableVal` branch unwraps the
// `{i8 hasValue; i32 payload}` box and runs the range comparison on the
// inner `i32`.
//
// `Main(x: int?)` is NOT a §33.2 entry-point signature (only `Main()` or
// `Main(args: Array<string>)` are); Helper.Get() returns `int?` so
// `valueProvesNonNull` returns false and `x` is NOT narrowed at the
// declaration — the `if x <> null then` block narrows `x` for the case
// form (§7.4; §19.1).
BEZEL_TEST(codegen_cg28,
           range_pattern_over_narrowed_nullable_value_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 7; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var x: int? := Helper.Get(); "
        "  if x <> null then "
        "    return (case x of null => 0, 5..10 => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// The same program with the range arm replaced by a guarded typed binding —
// §19.3's own prescribed spelling — still compiles and still selects the
// in-range arm, so the coverage #738 bought is kept, not dropped.
BEZEL_TEST(codegen_cg28, nullable_scrutinee_guarded_binding_replaces_range_3298) {
    BEZEL_EXPECT_EQ(run(
        "var x: int? := 5; "
        "var r := (case x of null => 0, var n: int when n >= 5 and n <= 10 => 1, "
        "_ => 2); "
        "return r;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var x: int? := null; "
        "var r := (case x of null => 0, var n: int when n >= 5 and n <= 10 => 1, "
        "_ => 2); "
        "return r;"), 0);
}

// ── fix #1233 / §19.3: range patterns with NEGATIVE lower bound ───────────
// Before the fix the parser parsed `case x -10..-6` with the scrutinee as
// the BINARY expression `x - 10` (additive prec 13 ≥ old threshold 12), so
// the actual range seen at codegen was `..-6` (only an upper bound). The
// fix raises the scrutinee parsing threshold to 14 (multiplicative) so that
// the unary-minus on the lower bound is NOT consumed as a binary minus with
// the scrutinee.

// §19.3 regression: positive range still works after the threshold change.
BEZEL_TEST(codegen_cg28, neg_range_pos_range_regression_1233) {
    // 7 ∈ [6..10] → 1
    BEZEL_EXPECT_EQ(run(
        "var x := 7; "
        "return (case x of 6..10 => 1, _ => 0);"), 1);
}
BEZEL_TEST(codegen_cg28, neg_range_pos_range_regression_miss_1233) {
    // 5 ∉ [6..10] → 0
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "return (case x of 6..10 => 1, _ => 0);"), 0);
}

// §19.3: closed negative range `lo..hi`; scrutinee -8 ∈ [-10..-6].
BEZEL_TEST(codegen_cg28, neg_range_inside_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -8; "
        "return (case x of -10..-6 => 1, _ => 0);"), 1);
}

// §19.3: lower bound inclusive: -10 ∈ [-10..-6].
BEZEL_TEST(codegen_cg28, neg_range_at_lower_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -10; "
        "return (case x of -10..-6 => 1, _ => 0);"), 1);
}

// §19.3: upper bound inclusive: -6 ∈ [-10..-6].
BEZEL_TEST(codegen_cg28, neg_range_at_upper_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -6; "
        "return (case x of -10..-6 => 1, _ => 0);"), 1);
}

// §19.3: value just above upper bound: -5 ∉ [-10..-6] → 0.
BEZEL_TEST(codegen_cg28, neg_range_above_upper_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -5; "
        "return (case x of -10..-6 => 1, _ => 0);"), 0);
}

// §19.3: positive value outside a negative range: 2 ∉ [-10..-6] → 0.
BEZEL_TEST(codegen_cg28, neg_range_pos_value_miss_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := 2; "
        "return (case x of -10..-6 => 1, _ => 0);"), 0);
}

// §19.3: half-open `..<`: upper bound is exclusive, so -6 ∉ [-10..<-6].
BEZEL_TEST(codegen_cg28, neg_range_half_open_upper_excluded_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -6; "
        "return (case x of -10..<-6 => 1, _ => 0);"), 0);
}

// §19.3: -7 ∈ [-10..<-6] (below exclusive upper).
BEZEL_TEST(codegen_cg28, neg_range_half_open_inside_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -7; "
        "return (case x of -10..<-6 => 1, _ => 0);"), 1);
}

// §19.3: multiple arms spanning negative and positive ranges; verify all
// selectors route correctly (multi-arm sum encoded as described in issue #1233).
// Arms: -10..-6 => 1, -5..5 => 2, 6..10 => 3, _ => 0.
BEZEL_TEST(codegen_cg28, neg_range_multi_arm_neg_arm_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := -8; "
        "return (case x of -10..-6 => 1, -5..5 => 2, 6..10 => 3, _ => 0);"), 1);
}
BEZEL_TEST(codegen_cg28, neg_range_multi_arm_mid_arm_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; "
        "return (case x of -10..-6 => 1, -5..5 => 2, 6..10 => 3, _ => 0);"), 2);
}
BEZEL_TEST(codegen_cg28, neg_range_multi_arm_pos_arm_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := 7; "
        "return (case x of -10..-6 => 1, -5..5 => 2, 6..10 => 3, _ => 0);"), 3);
}
BEZEL_TEST(codegen_cg28, neg_range_multi_arm_wildcard_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x := 11; "
        "return (case x of -10..-6 => 1, -5..5 => 2, 6..10 => 3, _ => 0);"), 0);
}

// §19.3: negative range on a `long` scrutinee (fix #1233 must apply at
// every integer width; litAtWidth must handle negative long literal bounds).
BEZEL_TEST(codegen_cg28, neg_range_long_inside_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x: long := -8L; "
        "return (case x of -10L..-6L => 1, _ => 0);"), 1);
}
BEZEL_TEST(codegen_cg28, neg_range_long_miss_1233) {
    BEZEL_EXPECT_EQ(run(
        "var x: long := -5L; "
        "return (case x of -10L..-6L => 1, _ => 0);"), 0);
}

// §19.3 / §16.3 (v0.277 / #3319): a range pattern over a `double` scrutinee
// is REJECTED at sema (Range<T>'s T must be integer or char). The pre-fix
// behaviour silently routed these through codegen via FCmp; that path is
// now unreachable. Negative-bounds are still covered on the integer path
// above (neg_range_long_inside_1233 / neg_range_long_miss_1233).
BEZEL_TEST(codegen_cg28, neg_range_double_inside_1233) {
    auto r = runAot(
        "var x: double := -3.0; "
        "return (case x of -5.0..-1.0 => 1, _ => 0);");
    // fix #3895 (issue #3892): see incompatible_value_arms_is_type_error above.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
BEZEL_TEST(codegen_cg28, neg_range_double_miss_1233) {
    auto r = runAot(
        "var x: double := 0.0; "
        "return (case x of -5.0..-1.0 => 1, _ => 0);");
    // fix #3895 (issue #3892): see incompatible_value_arms_is_type_error above.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ── fix #1234 / §19 / §9.4: DestructuringPattern on an Object/interface ──
// scrutinee — `Box(var v)` where the scrutinee is statically `Object`
// (a boxed record). Before the fix, `patternMatches` called
// `CreateExtractValue(scrut, ...)` directly on the box POINTER, which is
// illegal IR and aborted the compiler with an LLVM GEP assertion.
//
// The fix detects `scrut->getType()->isPointerTy()`, emits an isA type-test
// against the record's box descriptor, then conditionally unboxes (loads the
// record struct from the box payload at offset 2 words per §8.6) and runs the
// sub-pattern checks on the loaded struct value.

// §19 / §9.4 / §8.6: single-field record via `Object`; `var` binding.
// Expected: Box(42) → v = 42.
BEZEL_TEST(codegen_cg28_destructure, object_scrutinee_single_field_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box(v: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Box(42); "
        "  return (case o of "
        "    Box(var v) => v, "
        "    _ => 0); "
        "end Main; end C;"), 42);
}

// §19 / §9.4: two-field record via Object; bare binding names (no `var`).
// Expected: Point(3, 4) → x + y = 7.
BEZEL_TEST(codegen_cg28_destructure, object_scrutinee_two_fields_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Point(3, 4); "
        "  return (case o of "
        "    Point(x, y) => x + y, "
        "    _ => 0); "
        "end Main; end C;"), 7);
}

// §19 / §9.4: non-matching isA — the boxed record is a different type; the
// wildcard arm must fire.  Verifies the isA false-path in the new code.
BEZEL_TEST(codegen_cg28_destructure, object_scrutinee_isa_miss_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box(v: int); "
        "record Other(v: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Other(5); "
        "  return (case o of "
        "    Box(var v) => v, "
        "    _ => 99); "
        "end Main; end C;"), 99);
}

// §19 / §9.4: constant sub-pattern inside a boxed record: Box(7) matches
// an Object holding Box(7) and must NOT match Object holding Box(8).
BEZEL_TEST(codegen_cg28_destructure, object_scrutinee_const_subpat_match_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box(v: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Box(7); "
        "  return (case o of "
        "    Box(7) => 42, "
        "    _ => 0); "
        "end Main; end C;"), 42);
}
BEZEL_TEST(codegen_cg28_destructure, object_scrutinee_const_subpat_miss_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box(v: int); "
        "class C public static method Main(): int "
        "  var o: Object := new Box(8); "
        "  return (case o of "
        "    Box(7) => 42, "
        "    _ => 0); "
        "end Main; end C;"), 0);
}

// §19 / §9.4: regression — a directly-typed record scrutinee (not Object)
// must still work after the fix (the non-pointer path is unchanged).
BEZEL_TEST(codegen_cg28_destructure, direct_scrutinee_regression_1234) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  return (case p of "
        "    Point(x, y) => x + y, "
        "    _ => 0); "
        "end Main; end C;"), 30);
}

// ── fix #1256 / §19.4 / §17: parenthesised when-guard ────────────────────────
// A when-guard whose expression is parenthesised — `var t when (t > 0)` —
// was mis-parsed: the `(…)=>` token sequence triggered the lambda-detection
// lookahead (tokensLookLikeLambda), which returned true because the matching
// `)` is followed by the case-arm `=>` separator.  The parser then committed
// to a lambda parse and cascaded into errors.  The fix sets
// suppressTopLevelLambda_ before parsing the guard expression so only the
// outermost primary is affected; nested lambdas in call-arg position are
// unaffected (they enter parsePrimaryExpressionImpl with the flag cleared).

// §19.4/§17 (#1256): binding pattern + parenthesised guard — guard is true.
// `var t when (t > 0)` must parse as a binding pattern with guard `(t > 0)`.
// Scrutinee 5 > 0 → arm body `t` → returns 5.
BEZEL_TEST(codegen_cg28b, parenthesized_when_guard_1256) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "return (case x of var t when (t > 0) => t, _ => -1);"), 5);
}

// §19.4/§17 (#1256): parenthesised guard is false — falls through to `_`.
// Scrutinee -3 is not > 0; guard fails; wildcard yields -1. -1 does not fit an
// 8-bit exit code, so this test builds the full program directly and observes
// it on stdout (runAotOut) rather than going through the run(body) wrapper.
BEZEL_TEST(codegen_cg28b, parenthesized_when_guard_false_1256) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "var x := -3; "
        "Console.WriteLine((case x of var t when (t > 0) => t, _ => -1)); return 0; "
        " end Main; end C;"), "-1");
}

// §19.4/§17 (#1256): more complex parenthesised guard: `(t >= 1 and t <= 9)`.
// Exercises a compound boolean in parens — still a guard, not a lambda.
BEZEL_TEST(codegen_cg28b, parenthesized_compound_when_guard_1256) {
    BEZEL_EXPECT_EQ(run(
        "var x := 5; "
        "return (case x of var t when (t >= 1 and t <= 9) => t * 2, _ => 0);"), 10);
}
