// fix #3930 (§25.2/§11.8, v0.295): the arithmetic ("no applicable overload")
// diagnostic in `ExprTC::typeBinary` (ExprTypecheck_Exprs.cpp) named an
// unconstrained generic type parameter's operand type as the literal
// placeholder string `?` instead of the parameter's own name, and its
// suggested remedy ("declare `static method operator +(a: ?, b: ?)` inside
// `?`") was not actionable — a type parameter declares no members of its
// own (§11.8), so nothing can ever be declared "inside" one.
//
// ── ROOT CAUSE ───────────────────────────────────────────────────────────
//
// `typeName = lt->declaration ? std::string(lt->declaration->name) : "?";`
// — `lt->declaration` is null both for a bare, unconstrained type parameter
// AND for a null-declaration builtin (`Array<T>`, `IntPtr`, …); the pre-fix
// code folded both into "?" with no distinction. The sibling ordering-
// operator diagnostic a few hundred lines above this one (§25.2, v0.181,
// #1498) already got this right — it names the real parameter (`lp`) and
// suggests a `where T: INumber<T>` bound — but that fix was never carried
// over to the arithmetic path a different check further down produces.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// The arithmetic block now checks `isInScopeTypeParam(lt->headName())` (the
// SAME test the ordering diagnostic's own `typeParamNameOf` lambda uses)
// before falling into the generic Named-type wording, and gives the type-
// parameter case its own two wordings: `+`/`-`/`*`/`/` (all four declared
// together on `INumber<T>`, stdlib §2.19) recommend a `where T: INumber<T>`
// bound, exactly mirroring the ordering diagnostic; `mod` gets a SEPARATE
// wording because neither `INumber<T>` nor `ISteppable<T>` declares it —
// recommending either would be an actionable-looking remedy that does not
// actually work.
//
// ── WHAT EACH ROW IS FOR ─────────────────────────────────────────────────────
//
//   T1  the filed repro (`+` on an unconstrained `T`): names `T`, not `?`,
//       and suggests the real `INumber<T>` remedy.
//   T2  `-` on the same shape — the fix's own condition covers `+ - * /` in
//       one `if`; this rules out a fix that only patched the FIRST operator
//       token tested by the issue.
//   T3  `*` and `/` together (a two-method class) — same as T2, the other
//       two members of the four-way `if`.
//   T4  `mod` — the SEPARATE wording: still names the real parameter, but
//       does NOT suggest `INumber<T>`/`ISteppable<T>` (neither declares
//       `mod`) and instead points at the only real remedy, a user-declared
//       interface.
//   T5  ACCEPT-SIDE CONTROL: the exact T1 program, but with `where T:
//       INumber<T>` added — compiles AND computes the correct answer, not
//       merely "no longer rejected". Proves the fix is diagnostic-only and
//       does not touch the resolving path at all.
//   T6  REGRESSION CONTROL: a REAL declared type with no `+` overload keeps
//       the ORIGINAL "declare a static operator inside `TypeName`" wording,
//       naming the real type — proving `isInScopeTypeParam` does not
//       misfire on an ordinary null-declaration-free Named type and divert
//       it into the type-parameter wording. Uses an enum, not a class:
//       §25.1's OWN Record/Class/Choice arithmetic check, a few hundred
//       lines above this fix's own site, already intercepts every
//       Record/Class/Choice arithmetic case (with a DIFFERENT, order-aware
//       wording) before this diagnostic is ever reached — measured while
//       writing this row, which is exactly why this control exists.
//   T7  TWO DIFFERENT type parameters (`Foo<T, U>(a: T, b: U)`, `a + b`) —
//       the §11.8 same-parameter constraint-dispatch block above this fix's
//       own site requires BOTH operands to be the identical parameter, so
//       it never even attempts a lookup here; this exercises the fix's own
//       site directly (reached with `lt` = a type-parameter TypeRef whose
//       PAIRED operand is a different type, not merely "unresolved"), and
//       pins that it still names the LEFT operand's own parameter (`T`),
//       not `U` and not `?`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── T1 — `+` on an unconstrained type parameter names `T`, not `?`, with the
// actionable `INumber<T>` remedy.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           plus_unconstrained_names_type_parameter_spec_25_2_11_8) {
    auto r = runAot(
        "module M3930A; "
        "class Box<T> "
        "  public static method Add(a: T, b: T): T "
        "    return a + b; "
        "  end Add; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not available on type parameter `T`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "INumber<T>"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "inside `?`"));
}

// ── T2 — `-` on the same shape: the fix's own `if` covers all four
// arithmetic ops in one condition, not just `+`.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           minus_unconstrained_names_type_parameter_spec_25_2_11_8) {
    auto r = runAot(
        "module M3930B; "
        "class Box<T> "
        "  public static method Sub(a: T, b: T): T "
        "    return a - b; "
        "  end Sub; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `-` is not available on type parameter `T`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "INumber<T>"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── T3 — `*` and `/`, the remaining two members of the four-way `if`.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           star_and_slash_unconstrained_name_type_parameter_spec_25_2_11_8) {
    auto r = runAot(
        "module M3930C; "
        "class Box<T> "
        "  public static method Mul(a: T, b: T): T "
        "    return a * b; "
        "  end Mul; "
        "  public static method Div(a: T, b: T): T "
        "    return a / b; "
        "  end Div; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `*` is not available on type parameter `T`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `/` is not available on type parameter `T`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}

// ── T4 — `mod`: names the real parameter, but does NOT suggest
// `INumber<T>`/`ISteppable<T>` (neither declares `mod`) — a different,
// honest remedy (a user-declared interface).
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           mod_unconstrained_names_parameter_no_false_inumber_remedy_spec_25_2) {
    auto r = runAot(
        "module M3930D; "
        "class Box<T> "
        "  public static method Mod(a: T, b: T): T "
        "    return a mod b; "
        "  end Mod; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `mod` is not available on type parameter `T`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "declare a `static operator "
                                                    "mod(a: T, b: T): T`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
    // The dishonest remedy this row must NOT suggest: neither stdlib
    // interface declares `mod`, so recommending either would send the
    // reader down a path that does not work.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "where T: INumber<T>"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "where T: ISteppable<T>"));
}

// ── T5 — ACCEPT-SIDE CONTROL: the T1 program with `where T: INumber<T>`
// added compiles AND computes the correct answer — the fix is diagnostic-
// only, the resolving path is untouched.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           plus_constrained_inumber_compiles_correct_answer_spec_11_8) {
    auto r = runAot(
        "module M3930E; "
        "class Box<T> where T: INumber<T> "
        "  public static method Add(a: T, b: T): T "
        "    return a + b; "
        "  end Add; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Box<int>.Add(3, 4).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ── T6 — REGRESSION CONTROL: a REAL declared type (an enum — see the file
// header for why not a class/record/choice) with no matching `+` overload
// keeps the ORIGINAL wording, naming the real type — proves the new type-
// parameter branch does not misfire on an ordinary declared type.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           real_enum_no_overload_keeps_original_wording_spec_25) {
    auto r = runAot(
        "module M3930F; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var a := Color.Red; "
        "    var b := Color.Green; "
        "    var c := a + b; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not defined for type `Color`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "declare `static method operator +(a: Color, b: Color)` inside "
           "`Color`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type parameter"));
}

// ── T7 — TWO DIFFERENT type parameters (`T`, `U`): the §11.8 same-parameter
// constraint-dispatch block above never even looks this up (it requires
// BOTH operands to be the identical parameter), so this exercises the
// fix's own site directly and pins that it still names the LEFT operand's
// own parameter, not `U` and not `?`.
BEZEL_TEST(sema_arithmetic_typeparam_diagnostic_name_3930,
           two_different_type_parameters_names_left_operand_spec_25_2_11_8) {
    auto r = runAot(
        "module M3930G; "
        "class Box<T, U> "
        "  public static method Add(a: T, b: U): T "
        "    return a + b; "
        "  end Add; "
        "end Box; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "operator `+` is not available on type parameter `T`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type `?`"));
}
