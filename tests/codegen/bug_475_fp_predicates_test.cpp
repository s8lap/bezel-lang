// Fix verification tests for bug #475.
//
// #475 (MISCOMPILE / malformed IR): `double.IsNaN`, `double.IsInfinity`,
// `double.IsFinite`, `double.IsPositiveInfinity`, `double.IsNegativeInfinity`
// (and the `float` equivalents) had NO handler in `tryLowerPrimitiveStaticCall`.
// The call fell through to `lowerCall`'s method-symbol path, which returned
// nullptr for unrecognised static-primitive calls.  A null branch condition
// in `lowerIf` then took the "always-false" defensive path (unconditional jump
// to the else/skip block), so:
//
//   var nan := double.NaN;
//   if double.IsNaN(nan) then return 1; end;   // always skipped → returns 0
//   return 0;
//
// returns 0 instead of 1.  In a Debug build the null condition also caused
// malformed IR ("Branch condition is not 'i1' type!") that the always-on IR
// verifier reported.  Combining two predicates with `and`/`or` would crash
// with a PHINode::setIncomingValue assertion because the i32 literals produced
// by the fall-through path were incompatible with i1 PHI inputs.
//
// Root cause: `tryLowerPrimitiveStaticCall` (LlvmBackend_Calls.cpp) had no
// handlers for `IsNaN`, `IsInfinity`, `IsPositiveInfinity`,
// `IsNegativeInfinity`, or `IsFinite` on `P::Double` / `P::Float`.  Similarly,
// the sema member-access visitor (ExprTypecheck_Calls.cpp) only typed
// `IsNaN` and `IsInfinity` for static primitive accesses — the remaining three
// were untyped.  The instance-method paths (LlvmBackend_Exprs.cpp + sema) also
// lacked `IsPositiveInfinity` / `IsNegativeInfinity`.
//
// Fix:
//   • Sema (ExprTypecheck_Calls.cpp):
//       – static:   add `IsFinite`, `IsPositiveInfinity`, `IsNegativeInfinity`
//                   to the `double`/`float` primitive static-member typing block.
//       – instance: add `IsPositiveInfinity`, `IsNegativeInfinity` alongside
//                   the existing `IsNaN`/`IsInfinity`/`IsFinite` block.
//   • Codegen (LlvmBackend_Calls.cpp, tryLowerPrimitiveStaticCall):
//       – add a `pt == P::Double || pt == P::Float` guard for all five
//         predicates; lower each to the canonical inline `fcmp` sequence:
//           IsNaN            → fcmp uno %x, %x           (unordered self-compare)
//           IsInfinity       → (fcmp oeq %x, +inf) | (fcmp oeq %x, -inf)
//           IsPositiveInfinity → fcmp oeq %x, +inf
//           IsNegativeInfinity → fcmp oeq %x, -inf
//           IsFinite         → !(IsNaN | IsInfinity) = fcmp ord %x, %x
//                                && !(fcmp oeq %x, ±inf)
//         All produce i1 directly — no wrapping in i32 needed.
//   • Codegen (LlvmBackend_Exprs.cpp, instance-method dispatch):
//       – add `IsPositiveInfinity` / `IsNegativeInfinity` alongside the
//         existing `IsInfinity` instance-method handler.
//
// Spec: stdlib §9.2 (Double / Float):
//   public static method IsNaN(value: double): bool;
//   public static method IsInfinity(value: double): bool;
//   public static method IsPositiveInfinity(value: double): bool;
//   public static method IsNegativeInfinity(value: double): bool;
//   public static method IsFinite(value: double): bool;
//   (same set for Float)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── §9.2 double.IsNaN — stdlib: IsNaN(value: double): bool ───────────────────

// #475 exact reproducer: double.IsNaN(double.NaN) must return 1, not 0.
BEZEL_TEST(bug475_fp_predicates, double_IsNaN_nan_returns_true_475) {
    // stdlib §9.2: IsNaN(value: double): bool — NaN is not equal to itself.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if double.IsNaN(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNaN_finite_returns_false_475) {
    // stdlib §9.2: IsNaN on a finite value yields false.
    BEZEL_EXPECT_EQ(run(
        "if double.IsNaN(1.0) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNaN_zero_returns_false_475) {
    // stdlib §9.2: 0.0 is finite, not NaN.
    BEZEL_EXPECT_EQ(run(
        "if double.IsNaN(0.0) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNaN_stored_in_bool_var_475) {
    // stdlib §9.2: result is i1 (bool) — storable in a var.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "var b := double.IsNaN(nan); "
        "return (1 if b else 0);"), 1);
}

// ── §9.2 double.IsInfinity ────────────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, double_IsInfinity_posInf_returns_true_475) {
    // stdlib §9.2: IsInfinity(+Infinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var inf := double.PositiveInfinity; "
        "if double.IsInfinity(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsInfinity_negInf_returns_true_475) {
    // stdlib §9.2: IsInfinity(-Infinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var ninf := double.NegativeInfinity; "
        "if double.IsInfinity(ninf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsInfinity_finite_returns_false_475) {
    // stdlib §9.2: IsInfinity on a finite value → false.
    BEZEL_EXPECT_EQ(run(
        "if double.IsInfinity(42.0) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsInfinity_nan_returns_false_475) {
    // stdlib §9.2: NaN is not infinity.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if double.IsInfinity(nan) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 double.IsPositiveInfinity ───────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, double_IsPositiveInfinity_posInf_returns_true_475) {
    // stdlib §9.2: IsPositiveInfinity(+Infinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var inf := double.PositiveInfinity; "
        "if double.IsPositiveInfinity(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsPositiveInfinity_negInf_returns_false_475) {
    // stdlib §9.2: IsPositiveInfinity(-Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var ninf := double.NegativeInfinity; "
        "if double.IsPositiveInfinity(ninf) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsPositiveInfinity_finite_returns_false_475) {
    // stdlib §9.2: IsPositiveInfinity on a finite value → false.
    BEZEL_EXPECT_EQ(run(
        "if double.IsPositiveInfinity(3.14) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 double.IsNegativeInfinity ───────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, double_IsNegativeInfinity_negInf_returns_true_475) {
    // stdlib §9.2: IsNegativeInfinity(-Infinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var ninf := double.NegativeInfinity; "
        "if double.IsNegativeInfinity(ninf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNegativeInfinity_posInf_returns_false_475) {
    // stdlib §9.2: IsNegativeInfinity(+Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var inf := double.PositiveInfinity; "
        "if double.IsNegativeInfinity(inf) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNegativeInfinity_finite_returns_false_475) {
    // stdlib §9.2: IsNegativeInfinity on a finite value → false.
    BEZEL_EXPECT_EQ(run(
        "if double.IsNegativeInfinity(-5.0) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 double.IsFinite ─────────────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, double_IsFinite_finite_returns_true_475) {
    // stdlib §9.2: IsFinite on a finite value → true.
    BEZEL_EXPECT_EQ(run(
        "if double.IsFinite(1.0) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsFinite_nan_returns_false_475) {
    // stdlib §9.2: IsFinite(NaN) → false.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if double.IsFinite(nan) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsFinite_posInf_returns_false_475) {
    // stdlib §9.2: IsFinite(+Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var inf := double.PositiveInfinity; "
        "if double.IsFinite(inf) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsFinite_negInf_returns_false_475) {
    // stdlib §9.2: IsFinite(-Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var ninf := double.NegativeInfinity; "
        "if double.IsFinite(ninf) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 float.IsNaN ─────────────────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsNaN_nan_returns_true_475) {
    // stdlib §9.2: float.IsNaN(float.NaN) → true.
    BEZEL_EXPECT_EQ(run(
        "var nan := float.NaN; "
        "if float.IsNaN(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, float_IsNaN_finite_returns_false_475) {
    // stdlib §9.2: float.IsNaN on a finite float → false.
    BEZEL_EXPECT_EQ(run(
        "if float.IsNaN(1.0f) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 float.IsInfinity ────────────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsInfinity_posInf_returns_true_475) {
    // stdlib §9.2: float.IsInfinity(float.PositiveInfinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var inf := float.PositiveInfinity; "
        "if float.IsInfinity(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, float_IsInfinity_finite_returns_false_475) {
    // stdlib §9.2: float.IsInfinity on a finite float → false.
    BEZEL_EXPECT_EQ(run(
        "if float.IsInfinity(2.5f) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 float.IsPositiveInfinity ────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsPositiveInfinity_posInf_returns_true_475) {
    // stdlib §9.2: float.IsPositiveInfinity(float.PositiveInfinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var inf := float.PositiveInfinity; "
        "if float.IsPositiveInfinity(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, float_IsPositiveInfinity_negInf_returns_false_475) {
    // stdlib §9.2: float.IsPositiveInfinity(-Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var ninf := float.NegativeInfinity; "
        "if float.IsPositiveInfinity(ninf) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 float.IsNegativeInfinity ────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsNegativeInfinity_negInf_returns_true_475) {
    // stdlib §9.2: float.IsNegativeInfinity(float.NegativeInfinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var ninf := float.NegativeInfinity; "
        "if float.IsNegativeInfinity(ninf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, float_IsNegativeInfinity_posInf_returns_false_475) {
    // stdlib §9.2: float.IsNegativeInfinity(+Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var inf := float.PositiveInfinity; "
        "if float.IsNegativeInfinity(inf) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 float.IsFinite ──────────────────────────────────────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsFinite_finite_returns_true_475) {
    // stdlib §9.2: float.IsFinite(1.0f) → true.
    BEZEL_EXPECT_EQ(run(
        "if float.IsFinite(1.0f) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, float_IsFinite_nan_returns_false_475) {
    // stdlib §9.2: float.IsFinite(float.NaN) → false.
    BEZEL_EXPECT_EQ(run(
        "var nan := float.NaN; "
        "if float.IsFinite(nan) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, float_IsFinite_posInf_returns_false_475) {
    // stdlib §9.2: float.IsFinite(float.PositiveInfinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var inf := float.PositiveInfinity; "
        "if float.IsFinite(inf) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 combination tests — i1 combinability (#475 crash path) ───────────────

// #475: combining two predicates with `and` must compile without crashing
// (no PHINode::setIncomingValue assertion) and evaluate correctly.
BEZEL_TEST(bug475_fp_predicates, double_IsNaN_and_finite_compound_475) {
    // stdlib §9.2: NaN is both NaN and not finite.
    // `double.IsNaN(x) and (y > 0.0)` — both sides are i1, `and` is valid.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "var y := 1.0; "
        "if double.IsNaN(nan) and (y > 0.0) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsNaN_or_IsInfinity_compound_475) {
    // stdlib §9.2: `double.IsNaN(x) or double.IsInfinity(x)` — no PHI crash.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if double.IsNaN(nan) or double.IsInfinity(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_IsInfinity_or_IsFinite_compound_475) {
    // stdlib §9.2: `double.IsInfinity(inf) or double.IsFinite(inf)` — i1 PHI ok.
    BEZEL_EXPECT_EQ(run(
        "var inf := double.PositiveInfinity; "
        "if double.IsInfinity(inf) or double.IsFinite(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(bug475_fp_predicates, double_not_IsFinite_nan_compound_475) {
    // stdlib §9.2: (not double.IsFinite(NaN)) → true (NaN is not finite).
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if (not double.IsFinite(nan)) then return 1; end; "
        "return 0;"), 1);
}

// ── §9.2 predicate result stored in bool var and used in while ────────────────

BEZEL_TEST(bug475_fp_predicates, double_IsNaN_in_while_condition_475) {
    // stdlib §9.2: predicate usable as a while condition (i1 branch target).
    // This loop should not execute — count stays 0.
    BEZEL_EXPECT_EQ(run(
        "var count := 0; "
        "var x := 1.0; "
        "while double.IsNaN(x) do "
        "  count := count + 1; "
        "end; "
        "return count;"), 0);
}

BEZEL_TEST(bug475_fp_predicates, double_IsFinite_stored_in_bool_var_475) {
    // stdlib §9.2: result is a proper i1 bool — stored and retrieved correctly.
    BEZEL_EXPECT_EQ(run(
        "var b := double.IsFinite(3.14); "
        "return (1 if b else 0);"), 1);
}

// ── §9.2 float predicate combined with double comparison ─────────────────────

BEZEL_TEST(bug475_fp_predicates, float_IsNaN_or_double_IsNaN_compound_475) {
    // stdlib §9.2: mixing float and double predicates in a compound expression.
    BEZEL_EXPECT_EQ(run(
        "var fn := float.NaN; "
        "var dn := double.NaN; "
        "if float.IsNaN(fn) or double.IsNaN(dn) then return 1; end; "
        "return 0;"), 1);
}
