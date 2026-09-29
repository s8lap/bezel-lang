// fix #5110 (§22.6 / §25.2 / §6.6, v0.313): operator overload tie not
// diagnosed; primitive-target Named-RHS compound assignment kind-only check.
//
// Bug 1: findBinaryOperator (ExprTypecheck_Exprs.cpp) ran a two-pass search
//   (#5063) that treated a widening match as if it were exact, so `v + 100`
//   against `+(V, short)` and `+(V, long)` silently picked `long`. §22.6:
//   "two candidates that both require an implicit conversion at the same
//   position are equally ranked there, regardless of which conversion each
//   uses" and "If no candidate is unambiguously better ... the call is
//   ambiguous and is a compile error". The fix ranks each operand type's
//   applicable overloads per position (Exact / Converting) and reports the
//   tie (t4, t5, t7); exact-beats-convert and the explicit-cast remedy still
//   resolve (t6, t8; #5063's rows stay green).
//
// Bug 2: the primitive-target, Named-RHS compound-assignment branch
//   (ExprTypecheck.cpp) accepted ANY primitive target against a primitive
//   parameter (`p0->kind == Primitive`). It now asks §5.1's identity/widening
//   question via primitiveOperandMatchesParam (t9: `long += R` against
//   `operator +(int, R)` is rejected).

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"           // runAot / runAotChecked
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::parseAndCollect;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// Bug 1 — accept control: same-operand-type with a UNIQUE overload still works.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t1_same_type_unique_overload_accepted_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record W(V: int) "
        "  public static method operator +(w: W, n: int): W => "
        "new W(w.V + n); "
        "end W; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new W(10); "
        "    var b := a + 5; "
        "    Console.WriteLine(b.V); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "15\n");
}

// ---------------------------------------------------------------------------
// Bug 1 — accept control: cross-type overloads remain distinct (not ambiguous).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t2_cross_type_overloads_not_ambiguous_spec_25_2) {
    auto r = runAot(
        "module M; "
        "record L(V: int) "
        "  public static method operator +(l: L, n: int): L => "
        "new L(l.V + n); "
        "end L; "
        "record R(V: int) "
        "  public static method operator +(r: R, n: int): R => "
        "new R(r.V + n); "
        "end R; "
        "public class Program "
        "  public static method Main(): int "
        "    var l := new L(10); "
        "    var q := new R(20); "
        "    var a := l + 1; "
        "    var b := q + 2; "
        "    Console.WriteLine(a.V + b.V); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "33\n");
}

// ---------------------------------------------------------------------------
// Bug 2 — accept control: Named-target compound assign with applicable
// operator still works (§6.6 / §25.2).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t3_named_target_compound_assign_with_overload_accepted_spec_6_6) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public static method operator +(r: R, n: int): R => "
        "new R(r.X + n); "
        "end R; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new R(10); "
        "    r += 5; "
        "    Console.WriteLine(r.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "15\n");
}

// ---------------------------------------------------------------------------
// Bug 1 — REJECT pin: widen-vs-narrow ambiguity. `v + 100` against
// `+(V, short)` (constant narrowing §5.1) and `+(V, long)` (widening). Both
// require an implicit conversion at position 1 and neither is exact where
// the other converts — §22.6: "two candidates that both require an implicit
// conversion at the same position are equally ranked there, regardless of
// which conversion each uses." PRE-FIX (origin/main / bf2157ab): the call
// silently picked the first narrowing-pass candidate. The bf2157ab tie
// gate fired only when BOTH operands' types declared overloads; `int` has
// no user-defined `operator +`, so fromRight was null and the gate never
// engaged. POST-FIX: rejected with the §22.6 ambiguous-operator diagnostic.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t4_widen_vs_narrow_overload_ambiguous_rejected_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator +(v: V, n: short): V => "
        "new V(v.X + int(n) * 1000); "
        "  public static method operator +(v: V, n: long): V => "
        "new V(v.X + int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var w := v + 100; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous operator `+`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no applicable overload is unambiguously better"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, short)`, `operator +(V, long)`"));
}

// ---------------------------------------------------------------------------
// Bug 1 — REJECT pin: widen-vs-widen ambiguity (probeE's shape). `v + 100`
// against `+(V, long)` (int→long widening) and `+(V, float)` (int→float
// widening). Both candidates convert at position 1 and neither is exact —
// §22.6 Example 2's exact shape. PRE-FIX: silent pick of the first match.
// POST-FIX: rejected with the §22.6 diagnostic.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t5_widen_vs_widen_overload_ambiguous_rejected_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: double) "
        "  public static method operator +(v: V, n: long): V => "
        "new V(v.X + double(n) * 1000.0); "
        "  public static method operator +(v: V, n: float): V => "
        "new V(v.X + double(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3.0); "
        "    var w := v + 100; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous operator `+`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no applicable overload is unambiguously better"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, long)`, `operator +(V, float)`"));
}

// ---------------------------------------------------------------------------
// Bug 1 — ACCEPT control: when one candidate is exact where the other
// converts, the exact one wins — §22.6 "exact beats convert". `v + s` for
// `s: short` against `+(V, short)` (exact) and `+(V, long)` (short→long
// widening). PRE-FIX and POST-FIX: +(V, short) wins. Pins that the §22.6
// fix does not over-reject the unique-winner case.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t6_exact_match_over_widening_overload_accepted_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator +(v: V, n: short): V => "
        "new V(v.X + int(n) * 1000); "
        "  public static method operator +(v: V, n: long): V => "
        "new V(v.X + int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var s: short := 100; "
        "    var w := v + s; "
        "    Console.WriteLine(w.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100003\n");
}

// ---------------------------------------------------------------------------
// Bug 1 — REJECT pin, same tie through a type-declared both-sides search:
// the ambiguity is found among the LEFT operand type's overloads even though
// the right operand is a plain `int` variable (not a constant): `+(V, long)`
// and `+(V, double)` both widen `int` (§5.1) — §22.6 Example 2's shape.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t7_nonconstant_widen_vs_widen_ambiguous_rejected_spec_22_6_example_2) {
    auto r = runAot(
        "module M; "
        "record V(X: double) "
        "  public static method operator +(v: V, n: long): V => "
        "new V(v.X + 1.0); "
        "  public static method operator +(v: V, n: double): V => "
        "new V(v.X + 2.0); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3.0); "
        "    var a: int := 10; "
        "    var w := v + a; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no applicable overload is unambiguously better"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`operator +(V, long)`, `operator +(V, double)`"));
}

// ---------------------------------------------------------------------------
// Bug 1 — ACCEPT control: the explicit-cast remedy §22.6 names makes one
// candidate exact, and the constant-narrowed winner still runs at the
// parameter's width (#5063's re-typing survives the ranking).
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t8_explicit_cast_remedy_and_narrowed_winner_run_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record V(X: int) "
        "  public static method operator +(v: V, n: short): V => "
        "new V(v.X + int(n) * 1000); "
        "  public static method operator +(v: V, n: long): V => "
        "new V(v.X + int(n)); "
        "  public static method operator *(v: V, n: byte): V => "
        "new V(v.X * int(n)); "
        "end V; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new V(3); "
        "    var a := v + long(100); "
        "    var b := v + short(2); "
        "    var c := v * 7; "
        "    Console.WriteLine(a.X); "
        "    Console.WriteLine(b.X); "
        "    Console.WriteLine(c.X); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "103\n2003\n21\n");
}

// ---------------------------------------------------------------------------
// Bug 2 — REJECT pin: primitive target, Named RHS compound assignment.
// `n += r` with `n: long` against `operator +(n: int, r: R)`: `long` does
// not implicitly convert to `int` (§5.1), so the overload is not applicable
// and §6.6 makes the compound form a compile error. PRE-FIX the branch
// checked only `p0->kind == Primitive` and admitted it.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_operator_overload_tie_5110,
           t9_primitive_target_named_rhs_narrowing_param_rejected_spec_6_6) {
    auto r = runAot(
        "module M; "
        "record R(X: int) "
        "  public static method operator +(n: int, r: R): int => "
        "n + r.X; "
        "end R; "
        "public class Program "
        "  public static method Main(): int "
        "    var q := new R(5); "
        "    var n: long := 5_000_000_000L; "
        "    n += q; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`R` declares no applicable `operator +` overload (§25.2)"));
}
