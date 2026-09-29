// fix #5063 (§25.2 "the ordinary argument rules" / §5.1 "Constant narrowing",
// v0.307) — a user-defined binary operator's primitive-typed operand did not
// admit §5.1 constant narrowing, so a call-site-legal literal was rejected
// against an operator whose declared parameter is a narrower integer type.
//
// THE RULE. §25.2 (Restrictions and resolution): "a derived-typed operand
// satisfies a base-typed parameter by the ordinary argument rules" — the
// governing text for how an operator operand is matched against its
// declared parameter is the SAME rule as ordinary call-argument binding.
// §5.1 (Constant narrowing): "An expression of integer type that is a
// compile-time constant ... implicitly converts to any integer type whose
// range contains its value" and "participates in overload-resolution
// applicability (§22.6)" — with NO position carve-out ("there is no
// position carve-out"). So `v * 2` against `operator *(v: V, n: byte)`
// must be well-typed exactly as `Set(3)` is well-typed against
// `Set(level: byte)`.
//
// WHAT WENT WRONG (measured on d4f41e81, docs v0.307, before this fix, via
// the real driver): `findBinaryOperator`'s `operandMatchesParam`
// (ExprTypecheck_Exprs.cpp) admitted a primitive operand only when the
// types were identical or §5.1 numeric WIDENING applied — narrowing was
// never attempted. `v * 2` against ONLY `operator *(v: V, n: byte)`
// compiled to "operator `*` is not defined for type `V` with these
// operands — ... (§25)" — a false reject, not unsoundness (found by the
// non-author review of #5052, probe 1).
//
// THE FIX reuses `narrowIntConstantToTarget` (the ONE §5.1 constant-
// narrowing implementation, already used for call-argument binding) via a
// new shared helper `primitiveOperandMatchesParam`, from TWO call sites:
// `findBinaryOperator` (this file's T1-T6) and the compound-assignment
// operator search in `ExprTypecheck.cpp`'s `visitStmt` (T7-T9) — a SEPARATE
// implementation that does not route through `findBinaryOperator` at all.
// Both searches run TWO PASSES (narrowing disallowed, then allowed) so
// §22.6's "exact beats convert" ranking rule holds independent of
// declaration order — a single first-match-wins pass would let an
// earlier-declared narrowing candidate spuriously outrank a
// later-declared exact one (T5/T6/T9).
//
// A SECOND, PRE-EXISTING GAP was found while fixing the first: the
// compound-assignment path's OWN primitive-operand check
// (`ExprTypecheck.cpp` ~19567, pre-fix) accepted ANY primitive-typed RHS
// against ANY primitive parameter with NO width/kind check at all
// (`return operandTy->kind == TypeRef::Kind::Primitive;`) — so `v *= 2`
// against `operator *(v: V, n: byte)` was admitted with the literal still
// `int`-typed against an `i8` parameter (malformed IR at codegen; no
// downstream site performs the coercion for a named-target compound
// operator call). This fix closes that gap too, via the SAME shared
// helper (T7-T9). A behaviour-flip grep of `tests/` for any operator
// declaration with a narrow-integer-typed parameter found NONE (this is
// the first test exercising that shape), so no existing row relied on
// the old accept-everything primitive check.
//
// TEST MATRIX:
//   T1  the issue's own repro: `v * 2` against `operator *(v: V, n: byte)`
//       — accepted, RUNS, and computes the correct value.
//   T2  an out-of-range constant (`v * 300`, byte range 0..255) MUST STAY
//       REJECTED — §5.1: "A constant whose value does not fit the
//       target's range is a compile error at the conversion site."
//   T3  a non-constant `int` operand against the same byte parameter MUST
//       STAY REJECTED — §5.1: "a run-time `int` value still requires the
//       explicit `byte(x)` narrowing."
//   T4  the reversed operand order (`2 * v`) accepted when a matching
//       `operator *(n: byte, v: V)` is declared — narrowing applies at
//       EITHER operand position.
//   T5  ambiguity control: `operator *(V, byte)` declared BEFORE
//       `operator *(V, int)` — `v * 2` must still pick the EXACT `int`
//       overload (§22.6 "exact beats convert"), not the byte one that
//       narrowing would also admit.
//   T6  the same control with declaration order REVERSED (`int` before
//       `byte`) — proves the choice is not an artifact of search order.
//   T7  compound assignment `v *= 2` against `operator *(v: V, n: byte)`
//       — accepted, RUNS, correct value (the second, independent gap).
//   T8  compound-assignment out-of-range constant (`v *= 300`) MUST STAY
//       REJECTED, through the compound path's own search.
//   T9  compound-assignment ambiguity control, mirroring T5/T6 through the
//       compound path's own two-pass search.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_result_asserts.hpp"   // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// Measured (agent_check --probe, d4f41e81 + this fix): `v * 300` and `v * k`
// against a byte-only `operator *(v: V, n: byte)` both produce the
// no-applicable-candidate diagnostic, not a "no operator at all" one — the
// declaration exists, it just has no parameter the operand matches.
constexpr std::string_view kNoApplicableOverloadSpec25 =
    "operator `*` is not defined for type `V` with these operands";
constexpr std::string_view kNoApplicableOverloadSpec25CompoundHead =
    "declares no applicable `operator";
constexpr std::string_view kNoApplicableOverloadSpec252 =
    "(\xC2\xA7" "25.2)";

// Every row's compile must never be a crash/signal/IR-verifier abort — the
// exact class of pre-fix symptom §25 operator-typing bugs have produced
// elsewhere in this file family (binop_operand_typing_4807_test.cpp).
void expectCleanCompile(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "Assertion"));
}

}  // namespace

// ===========================================================================
// T1 — the issue's own repro. `v * 2` against `operator *(v: V, n: byte)`:
// accepted, and the computed value proves `n` really is `2`, not a
// degraded/garbage width.
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t1_byte_param_accepts_in_range_constant_spec_25_2_5_1) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var w := v * 2; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
    expectCleanCompile(r);
}

// ===========================================================================
// T2 — an out-of-range constant must stay rejected. §5.1: "A constant whose
// value does not fit the target's range is a compile error at the
// conversion site." 300 does not fit `byte` (0..255), so no candidate is
// applicable — the same "no applicable overload" verdict as no operator
// existing at all (§25).
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t2_out_of_range_constant_rejected_spec_5_1) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var w := v * 300; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kNoApplicableOverloadSpec25).c_str()));
    expectCleanCompile(r);
}

// ===========================================================================
// T3 — a non-constant `int` operand must stay rejected. §5.1: "a run-time
// `int` value still requires the explicit `byte(x)` narrowing" — constant
// narrowing applies to compile-time constants only.
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t3_non_constant_int_operand_rejected_spec_5_1) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var k: int := 2; "
        "    var w := v * k; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, std::string(kNoApplicableOverloadSpec25).c_str()));
    expectCleanCompile(r);
}

// ===========================================================================
// T4 — reversed operand order. Constant narrowing applies at EITHER operand
// position: `2 * v` against `operator *(n: byte, v: V)`.
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t4_reversed_operand_order_accepts_narrowed_constant_spec_25_2) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(n: byte, v: V): V => "
        "new V(int(n) * v.X); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(4); "
        "    var w := 2 * v; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\n");
    expectCleanCompile(r);
}

// ===========================================================================
// T5 — ambiguity control, byte declared FIRST. Two applicable-by-narrowing
// candidates exist (`byte` via narrowing, `int` exactly); §22.6's "exact
// beats convert" must select the EXACT `int` overload regardless of
// declaration order. The byte overload multiplies by 100 as a marker so a
// wrong dispatch is observable (600 vs the correct 6).
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t5_exact_int_overload_preferred_byte_declared_first_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n) * 100); "
        "  public static method operator *(v: V, n: int): V => "
        "new V(v.X * n); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var w := v * 2; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
    expectCleanCompile(r);
}

// ===========================================================================
// T6 — the same ambiguity control, declaration order REVERSED (`int` before
// `byte`). Proves T5's result is not an artifact of search/declaration
// order — the exact overload must win either way.
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t6_exact_int_overload_preferred_int_declared_first_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: int): V => "
        "new V(v.X * n); "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n) * 100); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var w := v * 2; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
    expectCleanCompile(r);
}

// ===========================================================================
// T7 — compound assignment. `v *= 2` against `operator *(v: V, n: byte)`:
// the SECOND, independent gap (the compound-assignment operator search in
// `ExprTypecheck.cpp` does not route through `findBinaryOperator`).
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t7_compound_assign_byte_param_accepts_constant_spec_6_6_5_1) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    v *= 2; "
        "    Console.WriteLine(v.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
    expectCleanCompile(r);
}

// ===========================================================================
// T8 — compound-assignment out-of-range constant must stay rejected,
// through the compound path's OWN search (mirrors T2).
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t8_compound_assign_out_of_range_constant_rejected_spec_5_1) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    v *= 300; "
        "    Console.WriteLine(v.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, std::string(kNoApplicableOverloadSpec25CompoundHead).c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, std::string(kNoApplicableOverloadSpec252).c_str()));
    expectCleanCompile(r);
}

// ===========================================================================
// T9 — compound-assignment ambiguity control, mirroring T5/T6 through the
// compound path's own two-pass search: `byte` declared first, `v *= 2` must
// still pick the exact `int` overload.
// ===========================================================================
BEZEL_TEST(sema_operator_constant_narrowing_5063,
           t9_compound_assign_exact_int_overload_preferred_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n) * 100); "
        "  public static method operator *(v: V, n: int): V => "
        "new V(v.X * n); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    v *= 2; "
        "    Console.WriteLine(v.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
    expectCleanCompile(r);
}
