// Regression test for #5189: §19.1 / §19.6 (v0.311, #5167) — a §7.4-narrowed
// scrutinee is non-nullable for exhaustiveness and §21.7 totality, and a
// `null` arm over a scrutinee that cannot hold `null` is admitted and never
// selected.
//
// THE DEFECT. caseArmsExhaustive
// (`src/sema/ExprTypecheck.cpp:2297`) and its three caller sites all keyed
// the nullable branch on `scrutTy->nullable` — the *declared* type — passing
// `ctx_.exprType(*scrutinee)` directly. §19.1 reads the scrutinee's
// nullability AFTER §7.4 narrowing at the `case` (the null fact alone), so
// the three sites were looking at the WRONG fact for every §7.4-narrowed
// scrutinee:
//
//   1. `ExprTypecheck.cpp:891` — stmt-position totality
//      (CaseStmt in stmtAlwaysExits).
//   2. `ExprTypecheck.cpp:2274` — expression-position
//      (CaseExpr via checkCaseExhaustiveness).
//   3. `ExprTypecheck.cpp:22094` — `noMatchEdgeStaticallyDead` cache
//      (CaseStmt at visitStmt).
//
// `operandStaticTypeAt` is the EXISTING narrowing helper, but
// it is primitive-only (`declared->kind != TypeRef::Kind::Primitive` returns
// early). For a record, enum, or choice-scrutinee the helper would have been
// a no-op, so a §7.4-narrowed record/enum/choice was keyed on the declared
// nullable bit and gave the WRONG verdict.
//
// The fix normalizes the scrutinee type through the §7.4 null fact
// (`isNarrowedNonNullRead`) at those three sites, for every kind, sharing
// the normalization with #5173's relational gate (the original issue notes
// the relational gate reads the same predicate, so one helper, not a fourth
// copy). A `null` arm over a non-nullable value scrutinee is admitted and
// lowered as constant `false` (the issue's precondition hazard: §8.4's
// null-safe equality on a non-nullable `i32` value is malformed IR at
// `ICmpEQ(scrut, ptr null)`).
//
// EVERY ROW is runAot — the issue pins each row to a specific runtime
// outcome. All cite §19.1 / §19.6 / §21.7, v0.311.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) §7.4 narrowed non-null on a record scrutinee in a value-returning
// method. §19.6 (record bullet): "the scrutinee's static type is a
// non-nullable record type and some unguarded arm matches every value of
// it." §19.1 narrows the declared type via §7.4 at the `case`, so the
// record scrutinee is non-nullable here and a value-returning method whose
// last return is this `case` is granted totality (§21.7 — "an
// exhaustive case statement whose arms all diverge has no reachable
// fall-through"). Before the fix the method was rejected as "does not
// return a value on every path": the scrutinee's declared type was `R?`,
// the exhaustive verdict looked at `nullable=true`, the no-match
// fall-through was deemed reachable, and §21.7's grant was withheld.
// ---------------------------------------------------------------------------
BEZEL_TEST(case_exhaust_narrowed_scrutinee_19_1_19_6_5189,
           record_stmt_case_returns_x_section_21_7) {
    BEZEL_EXPECT_EQ(run(
        "module M5189A; "
        "public record R5189(X: int); "
        "class App5189 "
        "  public static method f(r: R5189?): int "
        "    if r <> null then "
        "      case r of "
        "        R5189(var x) => return x; "
        "      end case; "
        "    end if; "
        "    return -1; "
        "  end f; "
        "  public static method Main(): int "
        "    var r: R5189? := new R5189(X := 42); "
        "    return App5189.f(r); "
        "  end Main; "
        "end App5189; "),
        42);
}

// ---------------------------------------------------------------------------
// (2) §19.6 enum bullet over a §7.4-narrowed enum scrutinee covering every
// constant. No non-exhaustiveness WARNING (§19.6 / §10.6) and the program
// runs. Pre-fix: scrutinee is `E?`, every constant named, but the verdict
// looked at the declared nullable bit and fired the warning.
// ---------------------------------------------------------------------------
BEZEL_TEST(case_exhaust_narrowed_scrutinee_19_1_19_6_5189,
           enum_expr_case_all_constants_no_warning_spec_19_6) {
    BEZEL_EXPECT_EQ(run(
        "module M5189B; "
        "public enum E5189 A := 1, B := 2, C := 3; end E5189; "
        "class App5189 "
        "  public static method g(e: E5189?): int "
        "    if e <> null then "
        "      return (case e of E5189.A => 1, E5189.B => 2, E5189.C => 3); "
        "    end if; "
        "    return 0; "
        "  end g; "
        "  public static method Main(): int "
        "    return App5189.g(E5189.B); "
        "  end Main; "
        "end App5189; "),
        2);
}

// ---------------------------------------------------------------------------
// (3) §19.6 nullable bullet: "A `null` arm over a scrutinee that cannot
// hold `null` … is admitted and never selected." `var i := 5;` is a
// non-nullable scrutinee; the `null` arm is admitted (no "this arm can
// never match" diagnostic) and `_` selects → 2. Pin the value: pre-fix, the
// scrutinee's declared type was `int` (non-nullable) so the program would
// have compiled here as a control — this row's purpose is the post-fix
// posture (the null arm is admitted, lowering the `null`-arm test as
// constant false; the issue's null-arm lowering hazard precondition).
// ---------------------------------------------------------------------------
BEZEL_TEST(case_exhaust_narrowed_scrutinee_19_1_19_6_5189,
           non_nullable_scrutinee_null_arm_selects_wildcard_spec_19_6) {
    BEZEL_EXPECT_EQ(run(
        "module M5189C; "
        "class App5189 "
        "  public static method h(): int "
        "    var i := 5; "
        "    return (case i of null => 1, _ => 2); "
        "  end h; "
        "  public static method Main(): int "
        "    return App5189.h(); "
        "  end Main; "
        "end App5189; "),
        2);
}

// ---------------------------------------------------------------------------
// (4) The alternative-list form of row (3). `null | 0` is a binding-free
// pattern alternative list (§19.3) over a non-nullable scrutinee. Both
// members never match (`i := 5`); `_` selects → 2. This exercises the
// `ConstantPattern(null)` parser path the issue names — distinct from the
// bare `null` arm in row (3).
// ---------------------------------------------------------------------------
BEZEL_TEST(case_exhaust_narrowed_scrutinee_19_1_19_6_5189,
           non_nullable_scrutinee_null_alt_in_pattern_spec_19_3_19_6) {
    BEZEL_EXPECT_EQ(run(
        "module M5189D; "
        "class App5189 "
        "  public static method i(): int "
        "    var j := 5; "
        "    return (case j of null | 0 => 1, _ => 2); "
        "  end i; "
        "  public static method Main(): int "
        "    return App5189.i(); "
        "  end Main; "
        "end App5189; "),
        2);
}

// ---------------------------------------------------------------------------
// (5) The generic row: `v: T` is NOT "cannot hold null" (§14.2 — only a
// class/interface constraint implies non-null). At `T = string?` the
// `null` arm IS selected (the null fact is NOT in force: no §7.4 narrowing
// exists for a bare generic type parameter). This pins the
// unconstrained-type-parameter half of the issue: the post-fix verifier
// does NOT spuriously narrow a `T` scrutinee, so a generic body that
// looks like the row (3) shape over `string?` STILL selects the null arm.
// ---------------------------------------------------------------------------
BEZEL_TEST(case_exhaust_narrowed_scrutinee_19_1_19_6_5189,
           unconstrained_type_param_null_arm_selected_at_string_q_14_2) {
    BEZEL_EXPECT_EQ(run(
        "module M5189E; "
        "class App5189 "
        "  public static method k<T>(v: T): int "
        "    return (case v of null => 1, _ => 2); "
        "  end k; "
        "  public static method Main(): int "
        "    var s: string? := null; "
        "    return App5189.k<string?>(s); "
        "  end Main; "
        "end App5189; "),
        1);
}