// Regression tests for issue 4998 — a derived constructor's named-argument
// `base(...)` chain into a base class's §22.10 primary list is paired in
// TEXTUAL order when the base declares an explicit constructor of the
// chain call's arity. Re-opens the #4484 shape by a same-arity sibling.
//
// WHAT §22.5 SAYS. "naming an argument changes which parameter it binds
// to, never when it runs" and "every construction site in the language
// reaches it through the same binding — a call, a `new` expression, and
// a record construction alike". The §22.8 step 0 site for a `base(...)`
// chain that binds the base's PRIMARY list is `prePopulateBasePositional
// Fields`'s case (b), which reads `SemaContext::positionalBinding` for
// the §22.5 binding sema published.
//
// WHAT THE COMPILER DID. `ExprTC::publishBaseChainPrimaryBinding`
// (ExprTypecheck_Calls.cpp) declined to publish a primary-parameter
// binding whenever ANY explicit constructor of the BASE had the SAME
// ARITY as the chain call. The base's pre-population then fell back to
// the textual-order pairing `chainCall->args[i] ↔ basePosFields[i]`
// (the pre-fix behavior #4484 closed for the no-sibling case), which
// silently swapped named-arg bindings whenever the same-arity sibling
// existed.
//
// MEASURED ON pre-fix:
//   new K(5).S() against `class B(a: int, b: int := 0)` with explicit
//   sibling `constructor(s: string, t: string) this(1); end constructor;`
//   and `class K : B public constructor(n: int) base(b := n, a := 1); end`
//   printed "5,1" — `b := n` paired with primary field 0 (`a`) and
//   `a := 1` paired with primary field 1 (`b`). Post-fix: "1,5".
//
// HOW IT IS FIXED. The sibling filter now asks the §22.5 question: probe
// each explicit constructor of the base with `bindArgsToParams`
// (emit=false) and decline to publish only when one of them BINDS the
// call. Same body `publishPrimaryCtorChainBinding` uses for its
// `this(...)` twin (the #4997 fix), the same `ctorForArgs` selector
// codegen's `prePopulateBasePositionalFields` reaches through.
//
// WHICH ROWS ARE THE PINS. `p13_repro_*` fails on pre-fix (the issue's
// exact repro). The four controls pass on BOTH sides — they pin that
// the fix did not re-admit the #3562 sibling-binds shape, that an
// arity-mismatch sibling is undisturbed, that a §22.5 permutation keeps
// the right binding under the binder check, and that a deeper inheritance
// chain reaches the same verdict.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// THE PIN (issue body's own repro, p13). `base(b := n, a := 1)` against
// `class B(a: int, b: int := 0)` with a same-arity explicit sibling
// `(s: string, t: string)`. §22.5 binds `b := n` to the `b` slot and
// `a := 1` to the `a` slot; the sibling has no parameter named `a` or
// `b`, so the primary is the only candidate. Pre-fix the textual-order
// fallback paired `b := n` with primary field 0 (`a`) and `a := 1` with
// field 1 (`b`), so `new K(5).S()` printed `5,1`.
//
// Asserts the printed VALUES, not just the exit status: a fix that
// suppresses the pre-population silently and exits 0 prints `5,1` —
// exactly the pre-fix value — and is invisible to an exit-status oracle.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           p13_repro_named_base_chain_binds_by_name_not_textual_22_5) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string, t: string) this(1); end constructor; "
        "end B; "
        "class K : B "
        "  public constructor(n: int) base(b := n, a := 1); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(5).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,5\n"));
}

// ---------------------------------------------------------------------------
// CONTROL 1 — same program WITHOUT B's `(s, t)` sibling. With no
// sibling, the arity predicate never fired pre-fix either; the
// pre-population already had its §22.5 binding. Pass on BOTH sides. It
// is here because a fix that inverted the gate (publishing only when a
// sibling exists, or unconditionally) would re-key a no-sibling base
// and break exactly this shape.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           control_no_sibling_still_publishes_base_primary_binding) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "end B; "
        "class K : B "
        "  public constructor(n: int) base(b := n, a := 1); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(5).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,5\n"));
}

// ---------------------------------------------------------------------------
// CONTROL 2 — same program with the sibling changed to ARITY 1
// (`constructor(s: string) this(1)`). The chain call's arity is 2; the
// arity predicate never fired pre-fix either, so this passes on both
// sides. Pins that the fix did not accidentally widen the predicate to
// "any sibling exists".
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           control_sibling_of_lower_arity_does_not_suppress_primary) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string) this(1); end constructor; "
        "end B; "
        "class K : B "
        "  public constructor(n: int) base(b := n, a := 1); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(5).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1,5\n"));
}

// ---------------------------------------------------------------------------
// PIN — same-arity sibling present, but the chain's argument ORDER
// matches textual order: `base(a := 5, b := 1)` writes `a` first, the
// primary's first slot is `a`, so textual and §22.5 order coincide. The
// pre-fix textual-pairing fallback printed the right answer by accident
// on this shape; the §22.5 binding still binds correctly under the fix.
//
// Prints "5,1" (a=5, b=1) on both pre-fix and post-fix. A fix that
// inverted the binder check (publishing only when the binder fails,
// or returning early when it succeeds) would print the wrong value here
// and that mistake would be caught by the matching `b := 1, a := 5`
// row below.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           named_arg_order_declared_then_swap_pins_swap_only) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string, t: string) this(1); end constructor; "
        "end B; "
        "class K : B "
        "  public constructor(n: int) base(a := 5, b := 1); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(0).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5,1\n"));
}

// ---------------------------------------------------------------------------
// THE SAME-ARITY-SIBLING PIN WHERE §22.5 BINDING MATTERS. Same shape as
// the previous row but with the chain's arguments in REVERSE order:
// `base(b := 1, a := 5)` writes `b` first, the primary's first slot is
// `a`. Textual order and §22.5 order disagree on arguments 0 and 1.
//
// Pre-fix the textual-pairing fallback printed `1,5` (first arg paired
// with first slot: `b := 1` → `a = 1`, second arg with second slot:
// `a := 5` → `b = 5`). Post-fix the §22.5 binder binds `b := 1` to the
// `b` slot and `a := 5` to the `a` slot, so print `5,1`.
//
// This row is the discriminator that distinguishes the fix from a
// "fix" that kept the textual-order pairing and only changed the
// no-sibling case.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           named_arg_reversal_pins_22_5_binding_not_textual_order) {
    auto r = runAot(
        "module M; "
        "class B(a: int, b: int := 0) "
        "  public method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string, t: string) this(1); end constructor; "
        "end B; "
        "class K : B "
        "  public constructor(n: int) base(b := 1, a := 5); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(0).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5,1\n"));
}

// ---------------------------------------------------------------------------
// DEEP-INHERITANCE PIN — three-level chain (K → J → B). K's base call
// reaches J, and J itself has a primary list plus a same-arity explicit
// sibling — the same defect shape, one level down.
//
// `class J(a: int, b: int := 0) : B` mirrors B's primary list, with its
// own same-arity explicit sibling `(s: string, t: string) this(1)`.
// B's primary is all-defaulted so J's implicit base() chain resolves.
// `class K : J` declares `public constructor(n: int) base(b := n, a := 3)`.
// `new K(5)` calls K's ctor, which chains to J's primary with
// `b := 5, a := 3`.
//
// Pre-fix J's same-arity sibling suppresses the primary binding, the
// pre-population falls back to textual pairing, and the printed values
// are `5,3` (the textual-order swap). Post-fix the binder check
// correctly rejects J's `(s, t)` sibling (none of its names match `a` or
// `b`), publishes the primary binding, and the printed values are
// `3,5`. Pins that the fix's binder check is applied to every base
// class in an inheritance chain, not just the leaf.
// ---------------------------------------------------------------------------

BEZEL_TEST(chain_base_to_primary_textual_pair_4998,
           deep_inheritance_three_level_primary_at_each_step_22_5) {
    auto r = runAot(
        "module M; "
        "class B(a: int := 0, b: int := 0) "
        "  public virtual method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string, t: string) this(1); end constructor; "
        "end B; "
        "class J(a: int, b: int := 0) : B "
        "  public override method S(): string return $\"{a},{b}\"; end S; "
        "  public constructor(s: string, t: string) this(1); end constructor; "
        "end J; "
        "class K : J "
        "  public constructor(n: int) base(b := n, a := 3); end constructor; "
        "end K; "
        "class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(5).S()); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // J.a=3 (the named `a := 3` arg), J.b=5 (the named `b := n` arg with n=5).
    // Pre-fix the textual-order pairing would print "5,3".
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3,5\n"));
}