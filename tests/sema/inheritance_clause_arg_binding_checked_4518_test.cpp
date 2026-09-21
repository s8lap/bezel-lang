// Regression tests for issue 4518 — §22.5 argument binding was UNCHECKED for
// a §22.10 INHERITANCE CLAUSE, so an under-supplied, over-supplied,
// doubly-bound or misnamed clause compiled with zero diagnostics and then
// either ran on a half-constructed base or SIGSEGVed
// (§22.5 "Optional parameters and named arguments" / §22.8 step 0 / §22.10
// "Primary constructors", v0.306).
//
// This is the CLAUSE twin of #4496, which closed the same gap on the
// `base(...)` chain arm. §22.10 is what makes the two one question:
//
//   "When a class with a primary constructor has a base class, the base call
//    appears in the inheritance clause … This replaces an explicit `base(...)`
//    call."
//
// so the clause IS the base call, and §22.5's two REJECTIONS reach it exactly
// as they reach the chain:
//
//   "Each supplied argument binds exactly one parameter and each parameter is
//    bound by at most one supplied argument: naming a parameter already bound
//    positionally …, or naming the same parameter twice, is a compile error
//    at the call naming that parameter"
//
//   "Every parameter that declares no default must be bound, positionally or
//    by name"
//
// and the section names the reach of the rule: "every construction site in
// the language reaches it through the same binding — a call, a `new`
// expression, and a record construction alike".
//
// WHAT THE COMPILER DID. `ExprTC::visitTopDecl`'s §22.10 clause walk ran the
// one §22.5 binder with `emit=false` (fix #4491), so a clause the binder
// DECLINED published no binding and lowered by textual position exactly as
// before. That `emit=false` was a deliberate, recorded decision in #4491 —
// whose subject was binding ORDER, not binding VALIDITY — and this file is
// that deferral closed.
//
// MEASURED ON 6c05b4cf (this fix's parent), against
// `class B(n: int, s: string)`. Every one compiled at exit 0 with ZERO
// diagnostics:
//
//   : B(n := v)               ran to exit 0 printing `4` and a blank line —
//                             the `s` slot kept its calloc zero, so the quiet
//                             failure mode is a half-constructed object whose
//                             NULL string an interpolating consumer
//                             dereferences.
//   : B(v, n := 2, s := "a")  ran and SIGSEGVed (exit 139) after printing `4`.
//   : B(v, "a", 3)            ran to exit 0 printing `4` / `a` — the third
//                             argument silently dropped.
//   : B(zz := v, s := "a")    ran to exit 0 printing `4` / `a` — the argument
//                             naming a parameter that does not exist silently
//                             bound slot 0 by position.
//
// The first two are silent MISCOMPILES, not merely missing diagnostics.
//
// ⚠ THE ORACLE IS THE SPECIFIC §22.5 MESSAGE, not `!r.ran` and deliberately
// not `failedAtCompileTimeWithRealLocation`. The pre-fix behaviour of the
// doubly-bound row is a CRASH, and a crash carries its own backstop
// diagnostic with a real location, so a location-shaped helper would answer
// true on BOTH sides of the fix and the row would pass vacuously (CLAUDE.md's
// catalogue of vacuous oracles records exactly this trap). The under-supplied
// row needs the message for the opposite reason: pre-fix it compiled AND ran
// to exit 0, so `!r.ran` is a real oracle there, and the message is what
// distinguishes "rejected for the §22.5 reason" from "rejected for something
// a later edit broke".
//
// THE CONTROLS MATTER AS MUCH AS THE PINS. This is a NEW REJECTION, so its
// risk is OVER-rejection. Five rows exercise shapes that MUST still compile
// and still produce the right values: a clause that legitimately OMITS a
// DEFAULTED base parameter (the shape a naive "every parameter must be bound"
// reading breaks, and which nothing else here would notice), the same shape
// with NAMES out of order (which also re-pins #4491's by-name binding through
// the now-loud binder), an all-positional exactly-supplied clause (the
// corpus's overwhelming majority shape), a well-formed named clause (#4491's
// own subject), and a clause that binds an EXPLICIT base constructor rather
// than the base's primary parameter list — the arm this check must NOT reach.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// PIN 1 — a required base parameter left UNBOUND. §22.5: "Every parameter
// that declares no default must be bound, positionally or by name".
//
// Pre-fix this compiled AND ran to exit 0, printing `4` and a blank line: no
// crash, no diagnostic, a half-constructed base.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           under_supplied_inheritance_clause_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(n := v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "required parameter `s` is not supplied (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — a parameter bound TWICE, positionally and then by name. §22.5:
// "naming a parameter already bound positionally … is a compile error at the
// call naming that parameter".
//
// Pre-fix this compiled at exit 0 and the BINARY SIGSEGVed (exit 139) after
// printing `4` — the string `"a"` stored into the 4-byte `n` slot's
// neighbour and the integer read back as a string pointer. That crash is why
// this row asserts the message rather than a location-shaped helper.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           inheritance_clause_binding_a_parameter_twice_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, n := 2, s := \"a\") "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `n` is bound more than once (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 3 — OVER-supplied. §22.5's arity half. Pre-fix the extra argument was
// silently dropped and the program ran to exit 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           over_supplied_inheritance_clause_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"a\", 3) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "supplies 3 argument(s) but the parameter list has 2 (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 4 — a named argument naming a parameter that does not exist. Pre-fix
// `zz := v` silently bound slot 0 by textual position and the program ran to
// exit 0 printing `4` / `a`, so the misnamed argument was not even inert: it
// supplied a parameter it never named.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           inheritance_clause_naming_a_nonexistent_parameter_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(zz := v, s := \"a\") "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "has no parameter named `zz` (§22.5)"));
}

// ---------------------------------------------------------------------------
// CONTROL — the OVER-REJECTION guard that matters most. §22.5: "an unbound
// optional parameter takes its default". `: B(v)` against
// `class B(n: int, s: string := "dflt")` binds slot 0 and legitimately omits
// the DEFAULTED slot 1. A "every parameter must be bound" reading — the naive
// way to implement this rejection — breaks exactly this, and the value
// assertion (not just `r.ran`) is what catches a fix that keeps compiling it
// but stops filling the default.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           clause_omitting_a_defaulted_base_parameter_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dflt\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ndflt\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the same omission with NAMES OUT OF ORDER, which is the shape
// where a loud binder and #4491's by-name store have to agree. `: B(k := 3,
// n := v)` against `class B(n: int, s: string := "dflt", k: int := 7)` binds
// slots 0 and 2 by name and omits the MIDDLE defaulted slot — so the supplied
// slots are not a prefix, and both the §22.5 per-slot default fill and the
// by-name store are exercised at once. Pre- and post-fix this prints
// `4` / `dflt` / `3`.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           named_clause_omitting_a_middle_defaulted_slot_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dflt\", k: int := 7) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(k := 3, n := v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ndflt\n3\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an ALL-POSITIONAL, exactly-supplied clause: the corpus's
// overwhelming majority shape, which must be untouched.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           all_positional_inheritance_clause_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"x\") "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(4); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a WELL-FORMED named clause, i.e. #4491's own subject. The binder
// must accept it AND still publish the by-name binding; a rejection here
// would silently undo #4491, and a binding lost here would silently restore
// its mispairing (which is why the row asserts `9` / `clause` rather than
// just `r.ran`).
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           well_formed_named_clause_still_accepted_and_bound_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(s := \"clause\", n := v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(9); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nclause\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a clause that binds an EXPLICIT base constructor rather than the
// base's primary parameter list. The publication site returns before the
// binder on that shape (an explicit non-static constructor of the clause's
// arity exists), so this check must not reach it: the base's primary list has
// TWO slots and the clause supplies ONE, which the binder would reject if it
// ran.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_arg_binding_checked_4518,
           clause_binding_an_explicit_base_ctor_unaffected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(a: int) "
        "    this(a, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(7); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nconv\n"));
}
