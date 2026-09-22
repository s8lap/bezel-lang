// Regression tests for issue 4496 — §22.5 argument binding was UNCHECKED for
// a `base(...)` constructor chain, so a doubly-bound or under-supplied base
// call compiled with zero diagnostics and then SIGSEGVed
// (§22.5 "Optional parameters and named arguments" / §22.8 step 0 / §22.10,
// v0.306).
//
// WHAT §22.5 REQUIRES, verbatim, both halves stated as REJECTIONS:
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
// WHAT THE COMPILER DID. `ExprTC::publishBaseChainPrimaryBinding` ran the one
// §22.5 binder with `emit=false`, so a call the binder DECLINED simply
// published no binding and lowered as it always had. That was a deliberate,
// recorded decision in #4484 (whose subject was binding ORDER, not binding
// VALIDITY) and this issue is that deferral closed. Note the asymmetry it
// leaves behind is now gone: the sibling `this(...)` arm has always run the
// same binder with `emit=true`.
//
// MEASURED ON e9239ab4 (the parent of this fix), against
// `class Base(n: int, s: string)`:
//
//   base(1, n := 2, s := "x")   bezelc -c exit 0, NO diagnostic. The binary
//                               printed `1` and died:
//                               "AddressSanitizer: SEGV on unknown address
//                                0x00000000000a … READ … bz_string_to_utf8z"
//                               — the integer 2 dereferenced as a string.
//   base(1)                     bezelc -c exit 0, NO diagnostic. The `s` slot
//                               kept its calloc zero, so `Show()` printed `1`
//                               and a blank line and exited 0; an
//                               interpolating consumer dereferences the NULL
//                               and SIGSEGVs.
//
// Both are silent MISCOMPILES, not merely missing diagnostics.
//
// ⚠ THE ORACLE IS THE SPECIFIC §22.5 MESSAGE, not `!r.ran` and not
// `failedAtCompileTimeWithRealLocation`. The pre-fix behaviour of row 1 is a
// CRASH, and a crash carries its own backstop diagnostic with a real
// location, so a location-shaped helper would answer true on BOTH sides of
// the fix and the row would pass vacuously (CLAUDE.md's catalogue of
// vacuous oracles records exactly this trap). Every reject row below pins the
// message text `bindArgsToParams` emits.
//
// ⚠ AND ROW 2's PRE-FIX BEHAVIOUR IS NOT A CRASH AT ALL — it compiles AND
// runs cleanly to exit 0. `!r.ran` alone is a real oracle for it, but the
// message assertion is what distinguishes "rejected for the §22.5 reason"
// from "rejected for some unrelated reason a later edit introduced".
//
// THE CONTROLS MATTER AS MUCH AS THE PINS. This fix is a NEW REJECTION, so
// the risk it carries is over-rejection, not under-rejection. Four rows
// exercise shapes that MUST still compile: a well-formed named `base(...)`
// (#4484's own subject), an all-positional one, one that legitimately OMITS a
// DEFAULTED parameter (the shape a naive "every parameter must be bound"
// reading would break), and one that binds an EXPLICIT base constructor
// rather than the base's primary parameter list.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// PIN 1 — a parameter bound TWICE, positionally and then by name. §22.5:
// "naming a parameter already bound positionally … is a compile error at the
// call naming that parameter".
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           base_call_binding_a_parameter_twice_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(1, n := 2, s := \"x\"); "
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
        r, "parameter `n` is bound more than once (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — a required parameter left UNBOUND. §22.5: "Every parameter that
// declares no default must be bound, positionally or by name".
//
// Pre-fix this row COMPILED AND RAN to exit 0, printing `1` and a blank line,
// so its pre-fix failure mode is the quiet one: no crash, no diagnostic, a
// half-constructed object.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           under_supplied_base_call_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(1); "
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
        r, "required parameter `s` is not supplied (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 3 — OVER-supplied. The third face of the same binder; §22.5's arity
// half.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           over_supplied_base_call_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(1, \"x\", 3); "
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
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "(§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 4 — a named argument naming a parameter that does not exist. §22.5's
// "a named argument names an existing parameter" half.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           base_call_naming_a_nonexistent_parameter_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(zz := 1, s := \"x\"); "
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
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "has no parameter named `zz` (§22.5)"));
}

// ---------------------------------------------------------------------------
// CONTROL — the OVER-REJECTION guard that matters most. §22.5: "an unbound
// optional parameter takes its default". `base(1)` against
// `class Base(n: int, k: int := 5)` binds slot 0 and legitimately omits the
// DEFAULTED slot 1. A "every parameter must be bound" reading — the naive way
// to implement this rejection — would break exactly this, and nothing else in
// the file would notice.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           base_call_omitting_a_defaulted_parameter_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, k: int := 5) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(1); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n5\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a WELL-FORMED named `base(...)`, i.e. #4484's own subject. The
// binder must accept it and still publish the by-name binding; a rejection
// that fired here would silently undo #4484.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           well_formed_named_base_call_still_accepted_and_bound_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(s := \"x\", n := 1); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an ALL-POSITIONAL, exactly-supplied `base(...)`. The overwhelming
// majority shape in the corpus; it must be untouched.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           all_positional_base_call_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(1, \"x\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a `base(...)` that binds an EXPLICIT base constructor, not the
// base's primary parameter list. The publisher returns before the binder on
// that shape (an explicit non-static constructor of the call's arity exists),
// so this check must not reach it: the base's primary list has TWO slots and
// the call supplies ONE, which the binder would reject if it ran.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_arg_binding_checked_4496,
           base_call_binding_an_explicit_base_ctor_unaffected_22_5) {
    auto r = runAot(
        "module M; "
        "class Base(n: int, s: string) "
        "  public constructor(a: int) "
        "    this(a, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end Base; "
        "class D : Base "
        "  public constructor() "
        "    base(7); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nconv\n"));
}
