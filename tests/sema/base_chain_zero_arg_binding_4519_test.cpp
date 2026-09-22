// Regression tests for issue 4519 — an EXPLICIT zero-argument `base()` into a
// base whose §22.10 primary parameter list has REQUIRED parameters was
// accepted silently, leaving the base subobject calloc-zero
// (§12.8 "Constructor chaining" / §22.5 "Optional parameters and named
// arguments" / §22.10 "Primary constructors", v0.306).
//
// This is the arity-zero hole #4496 left. That fix made
// `ExprTC::publishBaseChainPrimaryBinding` run the one §22.5 binder with
// `emit=true`, so an over-supplied, under-supplied, doubly-bound or misnamed
// `base(...)` became a compile error — but the publisher returned on
// `chain.args.empty()` BEFORE the binder ran, so `base()` never reached it.
//
// WHAT THE SPEC REQUIRES, and why this needs no ruling. §12.8 states the
// requirement in the same sentence that inserts the implicit chain:
//
//   "If neither `base(...)` nor `this(...)` is written, the compiler inserts
//    an implicit `base()` call (which requires the base class to have a
//    parameterless constructor; otherwise the chain must be explicit)."
//
// and §22.5 says what "parameterless" means for a primary constructor:
//
//   "Every parameter that declares no default must be bound, positionally or
//    by name"
//
// — so a primary constructor whose parameters ALL carry defaults IS callable
// with no arguments, and one with a required parameter is not. #4496 deferred
// this shape because "rejecting the explicit spelling without settling what
// the implicit one means in the same shape would make two spellings of one
// call disagree"; §12.8's parenthetical settles it — BOTH spellings require a
// parameterless base constructor.
//
// ⚠ SCOPE, STATED SO THE GAP IS NOT MISTAKEN FOR A RULING. This file covers
// the EXPLICIT `base()` spelling only. The IMPLICIT chain (`class D : B` with
// no constructor, or a derived constructor that writes no chain) is still
// accepted over the same base, and that is filed rather than pinned here: a
// test asserting the current acceptance would enshrine the defect. Its
// predicate is also wider than this one (it must reach a base whose only
// constructors are explicit, and settle accessibility and `abstract`), which
// is why it is a separate change.
//
// MEASURED ON 6c05b4cf (this fix's parent): the issue's own repro compiled at
// exit 0 with NO diagnostic, and the program printed `0` and a blank line and
// exited 0. Both of `B`'s required primary parameters kept their calloc zero;
// `s` is a NULL string pointer that `Console.WriteLine` tolerates and an
// interpolating consumer dereferences.
//
// ⚠ THE ORACLE IS THE SPECIFIC §22.5 MESSAGE. Pre-fix both pin rows COMPILED
// AND RAN to exit 0, so `!r.ran` is a real oracle for them — but the message
// is what distinguishes "rejected for the §22.5 reason" from "rejected
// because a later edit broke the zero-argument chain outright", which is
// exactly the regression this fix could cause.
//
// THE CONTROLS ARE THE POINT OF THE FILE. This is a new rejection on the
// single most common chain spelling in the corpus, so four rows pin shapes
// that MUST still compile and still produce the right values: an all-defaulted
// primary list, an explicit parameterless base constructor, a base with no
// primary parameter list at all, and a base with an EMPTY one.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// PIN 1 — the issue body's own repro. `base()` into `class B(n: int, s:
// string)`: two required parameters, neither bound.
//
// Pre-fix: compiled clean, printed `0` and a blank line, exited 0.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_into_required_primary_is_rejected_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
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
        r, "required parameter `n` is not supplied (§22.5)"));
}

// ---------------------------------------------------------------------------
// PIN 2 — ONE required parameter and one DEFAULTED one. This is what
// separates "the fix rejects `base()`" from "the fix rejects `base()` when a
// REQUIRED parameter is left unbound": the defaulted slot must not be part of
// the complaint, and the message must name `n`.
//
// Pre-fix: compiled clean and printed `0` / `5` — the defaulted slot took its
// default and the required one silently kept its calloc zero, which is the
// worse of the two failure modes because it reads as a working program.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_names_the_unbound_required_parameter_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int, k: int := 5) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(k.ToString()); "
        "  end Show; "
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
        r, "required parameter `n` is not supplied (§22.5)"));
}

// ---------------------------------------------------------------------------
// CONTROL — the OVER-REJECTION guard that matters most. §22.5 makes a primary
// constructor whose parameters ALL carry defaults callable with no arguments,
// so `base()` is exactly right here and both defaults must still be filled.
// The value assertion is load-bearing: a fix that keeps compiling this but
// stops filling the defaults leaves `0` and a NULL string, which `r.ran`
// cannot see.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_into_all_defaulted_primary_still_accepted_22_5) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"d\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nd\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — an EXPLICIT parameterless base constructor. §12.8's requirement
// is satisfied literally here, and the publisher returns above the binder on
// the arity match, so this check must not reach it even though the base's
// primary list has two REQUIRED parameters.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_to_an_explicit_parameterless_ctor_accepted_12_8) {
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nz\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a base with NO §22.10 primary parameter list at all: the ordinary
// `class B ... end B` with field initializers, over which `base()` is the
// §22.7 implicit parameterless constructor. The publisher returns on
// `!base->hasPositionalFields`, so this is untouched.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_into_a_base_with_no_primary_list_accepted_22_7) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 7; "
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a base with an EMPTY primary parameter list (`class B()`). It
// HAS positional fields as far as the parser is concerned
// (`hasPositionalFields` is set by the `(` alone), so it passes the guard the
// row above exercises and reaches the binder with zero formals and zero
// arguments — the one shape where the binder must say yes.
// ---------------------------------------------------------------------------

BEZEL_TEST(base_chain_zero_arg_binding_4519,
           zero_arg_base_call_into_an_empty_primary_list_accepted_22_10) {
    auto r = runAot(
        "module M; "
        "class B() "
        "  public n: int := 7; "
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
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}
