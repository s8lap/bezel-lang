// fix #3433 — a UNIT-typed argument in a constructor-CHAIN call was rejected
// (spec §4.7 *The unit type*, §12.8 *Constructor chaining*, §22.7
// *Constructors*, §22.10 *Primary constructors* — v0.283).
//
// MEASURED on 0c5ecd1 (this file's parent commit), with the issue's own repro:
//
//     class B  public constructor(u: ()) … end B;
//     class D: B  public constructor() base(C.Nothing()); … end D;
//
//       ->  :7:10: internal: lowerExpr returned null for base-constructor
//                  argument — #1386          (compile FAILS, exit 1)
//
// and on the inheritance-clause spelling of the same argument:
//
//     class D(): B(C.Nothing())
//       ->  :5:14: internal: lowerExpr returned null for positional
//                  base-constructor argument — #1386
//
// Before #3421 the first of these emitted malformed IR instead ("Call parameter
// type does not match function signature! … call void @bz$M.B$ctor(ptr %0,
// void <badref>)"); #3421 turned that into the located ICE above without
// closing the gap.
//
// ── WHY IT SHOULD COMPILE ────────────────────────────────────────────────────
//
// §4.7: "`()` is the type with exactly one value, also written `()`. It is the
// return type of methods declared without an explicit return type." and "The
// unit type is a value type". So `C.Nothing()` — a method declared with no
// return type — is a value of a value type.
//
// §12.8 *Constructor chaining* places no restriction whatever on the argument
// TYPES a `base(...)` / `this(...)` chain may carry; every rule it states is
// about which constructor runs and where the call may appear — "A derived
// class's constructor may chain to a base constructor", "`this(...)` chains to
// another constructor in the same class. At most one of `base(...)` or
// `this(...)` may appear, and only in the chaining position before the body."
// §22.7 says the same from the constructor's side: "Multiple constructors are
// permitted; chaining uses `this(...)` or `base(...)` in the chaining position
// before the body ([§12.8])."
//
// §22.10's inheritance clause is the third spelling of that one position, and
// §22.7 *Observing `this` during construction* says so explicitly — "In the
// **chaining position** (`base(...)`/`this(...)` arguments, a primary
// constructor's inheritance-clause base call)". That sentence is why T1, T4 and
// T5 belong in one test: the spec names them as one place, and the backend had
// them as two loops that both got it wrong.
//
// And the decisive evidence that this was a backend gap rather than a rule: the
// VERY SAME argument at the VERY SAME constructor already compiled and ran
// through the ordinary `new` form — `new B(C.Nothing())` — because that path
// goes through `buildOrderedArgs`, which has carried the §4.7 unit rescue since
// #2915/#3095/#3105/#3421. Only the chain spellings rejected it. T7 pins that
// control, so the fix cannot be shown green by a change that broke it.
//
// ── WHAT THE FIX IS ──────────────────────────────────────────────────────────
//
// `unitValueForTarget` is the ONE predicate #3421 established for "what does a
// unit-typed source lower to at a value-demanding location" (the placeholder
// byte for a `()` target, §4.7's PRESENT `{i8 1, i8 0}` for a `()?` one).
// Neither chain site could reach it, because neither goes through
// `buildOrderedArgs` — each lowers its own arguments. Both now resolve the
// chain TARGET's formal through the existing `paramTypeOf` authority and hand
// it to that same predicate, so the chain spelling and the `new` spelling agree
// by construction rather than by two copies staying in step.
//
// ── WHAT MAKES THIS A REGRESSION TEST AND NOT A SMOKE TEST ───────────────────
//
// The pre-fix failure is a hard compile error, so `r.ran` alone would pass on
// any program that merely compiles. Every case therefore pins the exact stdout.
// T3 pins that the `()?` formal receives §4.7's PRESENT value and not its
// `null` — a "fix" that stopped the ICE by passing a zero would read `null` and
// be silently wrong, and its own control (`new B(null)`, which prints `False`)
// proves the two are distinguishable. T6 is the EVALUATION-COUNT case: the
// argument's receiver and the argument itself must each run exactly ONCE, since
// the rescue is applied AFTER `lowerExpr` has already emitted the call's side
// effects and a second evaluation would print the marker twice.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

/* A method declared with NO return type — §4.7's "the return type of methods
   declared without an explicit return type". */
constexpr const char* kNothing =
    "  public static method Nothing() "
    "    Console.WriteLine(\"side\"); "
    "  end Nothing; ";

}  // namespace

// ── T1 — the issue's exact repro: `base(C.Nothing())` at a `()`-typed base
// constructor formal. Pre-fix: the #1386 ICE, compile fails. Post-fix the
// argument's side effect runs first, then the base body, then the derived body
// — §12.8: "A `base(...)` call, if present, appears before the constructor
// body".
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           base_chain_unit_call_arg_spec_4_7_12_8) {
    auto r = runAot(
        "module M3433a; "
        "class B "
        "  public constructor(u: ()) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(C.Nothing()); "
        "    Console.WriteLine(\"D-ctor\"); "
        "  end constructor; "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nB-ctor\nD-ctor\n"));
}

// ── T2 — the `()` LITERAL spelling of the same argument. §4.7 gives the type
// "exactly one value, also written `()`", so the literal and the call are two
// spellings of one value and the chain must accept both. The literal has always
// lowered to no llvm::Value, so this shape failed identically before the fix
// and its passing is not implied by T1's.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           base_chain_unit_literal_arg_spec_4_7_12_8) {
    auto r = runAot(
        "module M3433b; "
        "class B "
        "  public constructor(u: ()) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(()); "
        "    Console.WriteLine(\"D-ctor\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B-ctor\nD-ctor\n"));
}

// ── T3 — a `()?` base-constructor formal. §4.7: wrapping the unit type with
// `?` "produces an optional with two values, `null` and `()`", so the chain
// must deliver the PRESENT one, distinguishably. The `new B(null)` line is the
// control that makes `True` meaningful: it goes through the already-working
// `new` path and prints `False`, so a slot that read `null` either way could
// not produce this stdout.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           base_chain_unit_call_at_nullable_unit_formal_spec_4_7) {
    auto r = runAot(
        "module M3433c; "
        "class B "
        "  public constructor(u: ()?) "
        "    Console.WriteLine((u <> null).ToString()); "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(C.Nothing()); "
        "  end constructor; "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    using var b := new B(null); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nTrue\nFalse\n"));
}

// ── T4 — the `this(...)` sibling. §12.8 admits both delegations in the one
// chaining position and the backend lowers them through one emitter, so the
// same formal at the same site must accept the same argument; a fix keyed on
// the `base` spelling alone would leave this standing.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           this_chain_unit_call_arg_spec_4_7_12_8) {
    auto r = runAot(
        "module M3433d; "
        "class D "
        "  public constructor(u: ()) Console.WriteLine(\"D-1\"); "
        "  end constructor; "
        "  public constructor() "
        "    this(C.Nothing()); "
        "    Console.WriteLine(\"D-0\"); "
        "  end constructor; "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nD-1\nD-0\n"));
}

// ── T5 — the INHERITANCE-CLAUSE face: §22.10's `class D(): B(args)` — whose
// arguments §22.10 describes as flowing straight through ("their values flow
// directly into the field they initialize or the base argument they supply") —
// delegates to B's constructor through a DIFFERENT emitter loop, which
// reported its own "positional base-constructor argument — #1386". A fix at the
// explicit-chain site alone would not close it.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           inheritance_clause_unit_call_arg_spec_4_7_22_10) {
    auto r = runAot(
        "module M3433e; "
        "class B "
        "  public constructor(u: ()) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class D(): B(C.Nothing()) "
        "end D; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nB-ctor\n"));
}

// ── T6 — the EVALUATION COUNT (§12.8 chaining position). The rescue
// substitutes a value AFTER `lowerExpr`
// has already emitted the argument's IR, so nothing may be evaluated a second
// time. Both the argument's RECEIVER (`C.Mk()`) and the argument itself
// (`.Nothing()`) print a distinct marker, and the expected stdout admits
// exactly one of each: a re-lowering would print `RECV`/`ARG` twice, and a fix
// that dropped the call along with its non-existent result would print neither.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           base_chain_unit_arg_evaluated_exactly_once_spec_12_8) {
    auto r = runAot(
        "module M3433f; "
        "class R "
        "  public method Nothing() Console.WriteLine(\"ARG\"); end Nothing; "
        "end R; "
        "class B "
        "  public constructor(u: ()) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(C.Mk().Nothing()); "
        "    Console.WriteLine(\"D-ctor\"); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Mk(): owned R "
        "    Console.WriteLine(\"RECV\"); "
        "    return new R(); "
        "  end Mk; "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("RECV\nARG\nB-ctor\nD-ctor\n"));
}

// ── T7 — the CONTROL that settles the whole question: the same argument at the
// same constructor through the ordinary `new` form, which already compiled and
// ran before the fix. It is here so the two spellings are pinned together —
// they must not be allowed to drift apart again.
BEZEL_TEST(codegen_base_ctor_chain_unit_arg_3433,
           direct_new_unit_call_arg_control_spec_4_7_22_7) {
    auto r = runAot(
        "module M3433g; "
        "class B "
        "  public constructor(u: ()) Console.WriteLine(\"B-ctor\"); "
        "  end constructor; "
        "end B; "
        "class C " +
        std::string(kNothing) +
        "  public static method Main(): int "
        "    using var b := new B(C.Nothing()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("side\nB-ctor\n"));
}
