// Regression tests for issue 4549 — §12.8's IMPLICIT `base()` call was never
// checked against "the base class has a parameterless constructor", so a
// derived class over a base with required parameters compiled clean and was
// silently half-constructed (§12.8 "Constructor chaining" / §22.5 "Optional
// parameters and named arguments" / §22.7 / §22.10, v0.306).
//
// WHAT THE SPEC REQUIRES, and why this needs no ruling. §12.8 carries the
// requirement in the same sentence that inserts the chain:
//
//   "If neither `base(...)` nor `this(...)` is written, the compiler inserts
//    an implicit `base()` call (which requires the base class to have a
//    parameterless constructor; otherwise the chain must be explicit)."
//
// and §22.5 says what "parameterless" means for a §22.10 primary constructor:
//
//   "Every parameter that declares no default must be bound, positionally or
//    by name"
//
// — so a primary list (or an explicit constructor) whose parameters ALL carry
// defaults IS callable with no arguments, and one with a required parameter is
// not. #4519 closed the EXPLICIT zero-argument `base()` spelling on that same
// reading and filed this one; the two are the same call written two ways.
//
// ONE PREDICATE, BOTH SPELLINGS. The fix is
// `ExprTC::baseHasParameterlessConstructor` (ExprTypecheck_Calls.cpp), read by
// the implicit chain (`visitTopDecl`) AND by the explicit `base()` publisher.
// #4549 exists because those two disagreed: #4519 answered through the §22.5
// binder over the base's PRIMARY parameter list, which says nothing about a
// base whose only constructors are EXPLICIT. PIN 5 below is that residual, and
// it is in this file rather than #4519's precisely so the two spellings are
// pinned side by side and cannot drift apart again.
//
// MEASURED ON fcb4dbd5 (this fix's parent), each compiled with `build/bezelc`
// at exit 0 with NO diagnostic and RUN:
//
//   class D : B                     over `class B(n: int, s: string)`
//                                   printed `0` and a blank line, exit 0
//   class D(v: int) : B             same
//   class D : B + a constructor     same
//     writing no chain
//   class D : B                     over `class B` whose only constructor is
//                                   `constructor(a: int)` — printed `0`
//   constructor() base();           over that same base — printed `0`
//
// Both of `B`'s required primary parameters keep their calloc zero; `s` is a
// NULL string pointer that `Console.WriteLine` tolerates and an interpolating
// consumer (`$"n={n} s={s}"`) dereferences. These are silently HALF-
// CONSTRUCTED objects, not merely missing diagnostics.
//
// ⚠ THE ORACLE IS THE SPECIFIC §12.8 MESSAGE, not `failedAtCompileTimeWith
// RealLocation`. Pre-fix every pin COMPILED AND RAN to exit 0, so `!r.ran` is
// a real oracle here — but the message is what separates "rejected for the
// §12.8 reason" from "rejected because a later edit broke derivation
// outright", which is exactly the regression a new rejection this wide can
// cause. The two spellings carry two remedies (§12.8 names both), so PIN 5
// asserts the explicit spelling's own text.
//
// THE CONTROLS ARE THE POINT OF THE FILE. This rejection reaches every
// `class D : B` with no chain written, so eight rows pin shapes that MUST
// still compile — and each asserts the printed VALUES, because a fix that
// keeps compiling a shape but stops constructing its base is what an
// acceptance-only oracle cannot see. Three of them are deliberate DECLINES
// recorded in the predicate's comment (a private base constructor, an
// `abstract` base, and a base carrying both a primary list and explicit
// constructors): each compiles and runs correctly today, so rejecting it
// would be a new defect rather than a fix.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// `class B(n: int, s: string)` — two REQUIRED §22.10 primary parameters — plus
// a `Main` that constructs `D` and prints the base's state. The derived class
// is the only thing that varies across the pins.
std::string overRequiredPrimary(const std::string& derived,
                                const std::string& construct) {
    return "module M; "
           "class B(n: int, s: string) "
           "  public method Show() "
           "    Console.WriteLine(n.ToString()); "
           "    Console.WriteLine(s); "
           "  end Show; "
           "end B; " +
           derived +
           "class C "
           "  public static method Main(): int "
           "    using var d := " +
           construct +
           "; "
           "    d.Show(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// PIN 1 — the issue body's own repro: `class D : B` with NO constructor at
// all, over a base whose §22.10 primary list has two required parameters.
// §22.7's implicit parameterless constructor for `D` chains to `base()`, and
// §12.8 requires `B` to have a parameterless constructor for that.
//
// Pre-fix: compiled clean, printed `0` and a blank line, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_into_required_primary_is_rejected_12_8) {
    auto r = runAot(overRequiredPrimary("class D : B end D; ", "new D()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "implicit `base()` chain: base class `B` has no parameterless "
        "constructor; the chain must be explicit (§12.8)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — a derived class that DOES declare a constructor, which writes
// neither `base(...)` nor `this(...)`. §12.8's sentence is about the chain,
// not about the absence of a constructor, so this is the same defect reached
// by a different spelling and the diagnostic must land on the constructor.
//
// Pre-fix: compiled clean, printed `0` and a blank line, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           derived_constructor_writing_no_chain_is_rejected_12_8) {
    auto r = runAot(overRequiredPrimary(
        "class D : B "
        "  public k: int := 0; "
        "  public constructor() "
        "    k := 1; "
        "  end constructor; "
        "end D; ",
        "new D()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "implicit `base()` chain: base class `B` has no parameterless "
        "constructor; the chain must be explicit (§12.8)"));
}

// ---------------------------------------------------------------------------
// PIN 3 — a §22.10 primary-constructor derived class whose inheritance clause
// names the base but supplies NO argument list (`class D(v: int) : B`).
// §22.10 makes a clause WITH arguments the base call ("This replaces an
// explicit `base(...)` call"); a BARE clause writes no call, so §12.8 inserts
// one. This row is what pins the gate on `primaryBaseCtorArgs` being EMPTY
// rather than on the clause being absent.
//
// Pre-fix: compiled clean, printed `0` and a blank line, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           bare_inheritance_clause_on_a_primary_ctor_class_rejected_12_8) {
    auto r = runAot(overRequiredPrimary(
        "class D(v: int) : B "
        "  public method Go() Show(); end Go; "
        "end D; ",
        "new D(4)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "implicit `base()` chain: base class `B` has no parameterless "
        "constructor; the chain must be explicit (§12.8)"));
}

// ---------------------------------------------------------------------------
// PIN 4 — a base with NO §22.10 primary parameter list at all, whose only
// constructor is EXPLICIT and takes a required parameter. This is the form
// #4549's issue body names as wider than #4519's: the §22.5 binder over the
// base's positional fields has nothing to bind, so only a predicate that also
// reads the explicit constructors can answer.
//
// Pre-fix: compiled clean, printed `0`, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_into_explicit_ctor_only_base_rejected_12_8) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public constructor(a: int) "
        "    n := a; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "implicit `base()` chain: base class `B` has no parameterless "
        "constructor; the chain must be explicit (§12.8)"));
}

// ---------------------------------------------------------------------------
// PIN 5 — the EXPLICIT zero-argument `base()` over that SAME base. #4519
// closed the explicit spelling through the §22.5 binder over the base's
// PRIMARY parameter list, and this base has none, so the publisher returned
// above the binder and the call stayed silent. The shared predicate is what
// closes it, and this row sits beside PIN 4 so the two spellings of ONE call
// are pinned to the same verdict on the same base.
//
// The remedy differs — an explicit `base()` cannot be "made explicit" — so
// this asserts the explicit spelling's own text.
//
// Pre-fix: compiled clean, printed `0`, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           explicit_zero_arg_base_into_explicit_ctor_only_base_rejected_12_8) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public constructor(a: int) "
        "    n := a; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B "
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "`base()` chain: base class `B` has no parameterless constructor; "
        "supply its arguments (§12.8)"));
}

// ---------------------------------------------------------------------------
// CONTROL — the OVER-REJECTION guard that matters most, and the one a naive
// "the base must declare a parameterless constructor" reading breaks. §22.5
// makes an ALL-DEFAULTED §22.10 primary list callable with no arguments, so
// the implicit chain is exactly right here and both defaults must still be
// filled. The value assertion is load-bearing: a fix that keeps compiling
// this but stops filling the defaults prints `0` and a NULL string, which
// `r.ran` cannot see.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_into_all_defaulted_primary_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"d\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nd\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the same §22.5 reading on an EXPLICIT constructor whose single
// parameter carries a default. The predicate must run the binder over the
// constructor's formal list too, not compare its arity against zero.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_into_all_defaulted_explicit_ctor_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public constructor(a: int := 7) "
        "    n := a; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a base carrying BOTH a required §22.10 primary list AND an
// explicit parameterless constructor. §22.10 requires that constructor to
// chain via `this(...)`, so `base()` reaches the primary through it and the
// object is fully constructed. A predicate that looked only at the primary
// list would reject this working program.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_to_an_explicit_parameterless_ctor_accepted_12_8) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor() "
        "    this(1, \"z\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nz\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a base with an EMPTY primary list, `class B()`. The parser marks
// `hasPositionalFields` on the `(` alone, so this reaches the predicate's
// arm (1) with zero formals; the binder accepts an empty argument list
// against an empty formal list, which is the right answer.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           implicit_base_into_empty_primary_list_accepted_22_10) {
    auto r = runAot(
        "module M; "
        "class B() "
        "  public n: int := 7; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — DECLINED: ACCESSIBILITY. A `private constructor()` on the base is
// what the predicate finds, and it accepts. This compiles and runs correctly
// today (measured on fcb4dbd5: prints `5`), so rejecting it here would be a
// new defect. Whether §12.5 should make it unreachable from a derived class
// is a separate, wider question than §12.8's requirement, recorded in the
// predicate's comment rather than decided here.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           private_parameterless_base_ctor_still_accepted_declined_12_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  private constructor() "
        "    n := 5; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — DECLINED: `abstract`. §12.5 forbids `new` on an abstract class;
// being a BASE is what one is for, so the modifier says nothing about §12.8's
// requirement. The base here declares neither a constructor nor a primary
// list, so §22.7's implicit parameterless constructor is what the chain
// reaches — the predicate's arm (3).
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           abstract_base_with_implicit_parameterless_ctor_accepted_22_7) {
    auto r = runAot(
        "module M; "
        "abstract class B "
        "  public n: int := 4; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D : B end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a §22.10 inheritance clause WITH arguments. §22.10: "the base call
// appears in the inheritance clause … This replaces an explicit `base(...)`
// call", so §12.8 inserts nothing and this check must not reach it even
// though `B`'s primary parameters are required. The values pin that the
// clause still constructs the base.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           inheritance_clause_with_arguments_unaffected_22_10) {
    auto r = runAot(overRequiredPrimary(
        "class D(v: int) : B(v, \"x\") "
        "  public method Go() Show(); end Go; "
        "end D; ",
        "new D(4)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a derived constructor that WRITES its `base(...)` chain. §12.8
// inserts nothing when one is written, so the check must not reach it. This
// is the corpus's dominant shape for a base with required parameters.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           derived_constructor_with_an_explicit_base_chain_unaffected_12_8) {
    auto r = runAot(overRequiredPrimary(
        "class D : B "
        "  public constructor(k: int) "
        "    base(k, \"x\"); "
        "  end constructor; "
        "end D; ",
        "new D(4)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a base that is a BUILT-IN exception type. §31 exception bases have
// no `TypeSymbol` in `baseTypes` at all, so the predicate is never asked; this
// row pins that the new rejection does not reach the one class of base whose
// constructor list sema cannot see.
// ---------------------------------------------------------------------------

BEZEL_TEST(implicit_base_parameterless_4549,
           builtin_exception_base_unaffected_31_1) {
    auto r = runAot(
        "module M; "
        "class MyEx : Exception "
        "  public constructor(m: string) "
        "    base(m); "
        "  end constructor; "
        "end MyEx; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}
