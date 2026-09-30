// Regression tests for issue 4520 — a §22.10 INHERITANCE CLAUSE that binds an
// EXPLICIT base constructor was paired by TEXTUAL POSITION, twice over, so a
// named clause lost its arguments entirely
// (§22.5 "Optional parameters and named arguments" / §22.8 step 1 / §22.10
// "Primary constructors", v0.306).
//
// This is the THIRD and last site of the family #4484 and #4491 fixed. #4484
// closed the `base(...)` chain arm and #4491 the clause bound to the base's
// PRIMARY parameter list, both inside
// `Emitter::prePopulateBasePositionalFields`. This one is a different
// function (`Emitter::emitImplicitBase`), a different resolution path
// (`baseClauseExplicitCtor` resolves an `ast::ConstructorDecl` and CALLS it)
// and a different formal source (the constructor's own parameter list, not
// the base's positional fields).
//
// WHAT §22.5 AND §22.10 REQUIRE. §22.5 twice: "naming an argument changes
// which parameter it binds to, never when it runs", and — naming the reach of
// the rule — "every construction site in the language reaches it through the
// same binding — a call, a `new` expression, and a record construction
// alike". §22.10 puts the clause among them: "When a class with a primary
// constructor has a base class, the base call appears in the inheritance
// clause … This replaces an explicit `base(...)` call."
//
// WHAT THE COMPILER DID, and it was TWO positional pairings, not one:
//
//   1. `emitImplicitBase`'s argument loop paired `clsBaseArgs[i]` with the
//      constructor's parameter `i` and never read `arg.name`.
//   2. `ctorForArgs`'s `ctorTypeMatches` — the RESOLVER — compares
//      `args[i]`'s type class with parameter `i`'s, so it rejected the
//      constructor outright whenever the names were right and the order was
//      not.
//
// (2) is why the observable is a DROP rather than a mispaired store: the
// resolver answered "no such constructor", `emitImplicitBase` fell through to
// its arity-0 fallback, found no parameterless constructor either, and the
// base subobject was never constructed. Re-keying (1) alone would not have
// moved these programs one byte.
//
// MEASURED ON 6c05b4cf (this fix's parent): the issue's own repro,
// `class D(v: int) : B(b := "e", a := v)` over
// `B.constructor(a: int, b: string)`, compiled at exit 0 with NO diagnostic
// and printed `0` and a blank line — `B`'s declared field initializers,
// untouched — where `4` / `e` is correct. The all-positional spelling of the
// same program printed `4` / `e`, so the clause plumbing worked and it was
// the NAMES that were lost.
//
// HOW IT IS FIXED. Not with a third copy of §22.5 in codegen. Sema publishes
// the clause's binding against THIS constructor's formal list through the ONE
// binder body every construction site runs (`bindArgsToParams` over
// `FormalsView::of(*ctor)`), on a channel of its own — the sibling table
// describes the base's PRIMARY parameter list, and serving that to a call
// that binds a convenience constructor's list is exactly the mispairing
// #3562 removed. Codegen reads it in two places: `baseClauseExplicitCtor`
// adopts sema's constructor when `ctorForArgs` finds none for a NAMED clause,
// and the argument loop places each lowered value at the formal §22.5 binds
// it to. The resolved `ConstructorSymbol` travels with the binding and the
// consumer checks pointer identity, so a sema/codegen disagreement costs the
// fix, never a wrong-slot store.
//
// ⚠ THE PERMUTATIONS ARE NOT INTERCHANGEABLE. `[1, 0]` is SELF-INVERSE and so
// cannot tell a `paramToArg` map from its inverse — the most plausible way to
// get this wrong. Only `three_cycle_*` (`[1, 2, 0]`) has that discriminating
// power; do not "simplify" it into another two-argument swap.
//
// ⚠ TWO ROWS EXIST FOR THE RE-KEYED RELEASE LOOPS, and they are not
// decoration. `emitImplicitBase` runs two post-call passes over the clause's
// arguments — #3759's +1-string release and #4077's closure release — and
// BOTH indexed `args` (or `clsBaseArgs`) positionally. A miskeyed release is
// invisible to a value oracle: it leaks, or frees the wrong slot. Both rows
// are `runAotChecked` (ASan/LSan on the generated program) and both amplify
// the construction into a loop, because a one-shot leak can stay reachable
// from a live stack slot and pass on BOTH sides.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// PIN 1 — the issue body's own repro, verbatim. `: B(b := "e", a := v)`
// against `constructor(a: int, b: string)`.
//
// Asserts the printed VALUES. Pre-fix the program compiled AND ran to exit 0,
// so an exit-status oracle sees nothing at all here.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           reversed_named_clause_to_explicit_base_ctor_binds_by_name_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string) "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(b := \"e\", a := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ne\n"));
}

// ---------------------------------------------------------------------------
// PIN 2 — the 3-CYCLE, and the only row with the power to tell a
// `paramToArg` map from its inverse. `: B(pc := 3, pa := 1, pb := 2)` against
// `constructor(pa, pb, pc)` binds the permutation [1, 2, 0], whose inverse
// [2, 0, 1] is a DIFFERENT map — reading the table backwards yields `3 1 2`.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           three_cycle_named_clause_to_explicit_base_ctor_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public a: int := 0; "
        "  public b: int := 0; "
        "  public c: int := 0; "
        "  public constructor(pa: int, pb: int, pc: int) "
        "    a := pa; "
        "    b := pb; "
        "    c := pc; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b.ToString()); "
        "    Console.WriteLine(c.ToString()); "
        "  end Show; "
        "end B; "
        "class D() : B(pc := 3, pa := 1, pb := 2) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n"));
}

// ---------------------------------------------------------------------------
// PIN 3 — a 16-byte `decimal` bound BY NAME into what textual order makes the
// trailing 4-byte `int` formal, under ASan. This is the row that would catch
// a re-keying which resolves the constructor correctly but then stores each
// value at the WRONG formal: an 8-byte `string`-into-`int` store lands inside
// a valid shadow granule and is silent, a 16-byte one is reported at the
// store (the reason #4484's and #4491's files both carry a decimal row).
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           reversed_named_decimal_clause_to_explicit_base_ctor_22_5) {
    auto r = runAotChecked(
        "module M; "
        "class B "
        "  public d: decimal := 0m; "
        "  public n: int := 0; "
        "  public constructor(pd: decimal, pn: int) "
        "    d := pd; "
        "    n := pn; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(d.ToString()); "
        "    Console.WriteLine(n.ToString()); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(pn := v, pd := 7.5m) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var x := new D(1); "
        "    x.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7.5\n1\n"));
}

// ---------------------------------------------------------------------------
// PIN 4 — §22.5 *Evaluation order*: "A call's supplied argument expressions
// are evaluated in call-site textual order — left to right as written, for
// positional and named arguments alike; naming an argument changes which
// parameter it binds to, never when it runs."
//
// This is the justification for keeping the lowering loop SOURCE-indexed and
// re-keying only the slot each value lands in, and nothing else in the file
// protects it: a "simplification" that lowered in parameter order would keep
// every other row green and silently invert the observable side-effect order.
// `: B(b := Log.B(), a := Log.A())` — the textually first argument runs
// first (AtB=1, AtA=2), and still binds by name (n=1, s=x).
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           named_clause_to_explicit_base_ctor_evaluates_in_textual_order_22_5) {
    auto r = runAot(
        "module M; "
        "class Log "
        "  public static Seq: int := 0; "
        "  public static AtA: int := 0; "
        "  public static AtB: int := 0; "
        "  public static method A(): int "
        "    Seq := Seq + 1; "
        "    AtA := Seq; "
        "    return 1; "
        "  end A; "
        "  public static method B(): string "
        "    Seq := Seq + 1; "
        "    AtB := Seq; "
        "    return \"x\"; "
        "  end B; "
        "end Log; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string) "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D() : B(b := Log.B(), a := Log.A()) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.Go(); "
        "    Console.WriteLine(Log.AtA.ToString()); "
        "    Console.WriteLine(Log.AtB.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // n=1  s=x  AtA=2 (ran second)  AtB=1 (ran first)
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n2\n1\n"));
}

// ---------------------------------------------------------------------------
// PIN 5 — the #3759 +1-STRING RELEASE, re-keyed. That post-call pass walked
// the clause's SOURCE arguments and released `args[ai + 1]`, i.e. the value
// sitting at FORMAL slot `ai`. With a named clause those are different slots,
// so the release would reach an `int` (a wild `bz_string_release` on a
// non-pointer) or miss the producer entirely (a leak).
//
// `runAotChecked` + a 200-iteration loop: a one-shot leak can stay reachable
// from a live stack slot and pass on both sides, which is why the loop is
// part of the oracle rather than decoration.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           named_clause_string_producer_released_at_its_bound_slot_3759) {
    auto r = runAotChecked(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(pn: int, ps: string) "
        "    n := pn; "
        "    s := ps; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(ps := $\"v{v}\", pn := v) "
        "  public method Go() Show(); end Go; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var d := new D(4); "
        "      i := i + 1; "
        "    end; "
        "    using var e := new D(4); "
        "    e.Go(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nv4\n"));
}

// ---------------------------------------------------------------------------
// PIN 6 — the #4077 CLOSURE RELEASE, re-keyed. That pass walks the base
// constructor's FORMALS looking for a function-typed one whose argument is a
// +1 closure producer, and read `clsBaseArgs[pi]` — the argument at the
// formal's own index. Here the function-typed formal is FIRST and the lambda
// is written SECOND, so the old indexing looks at `pn := x`, finds no closure
// producer, and never releases the closure cell + env + capture cells: a leak
// per construction, invisible to every value oracle.
//
// 500 iterations under ASan/LSan, and the printed `18` (the constructor
// actually ran, with both arguments at their bound formals) plus `0` (every
// `Res` disposed) are what separate "released correctly" from "never called
// at all", which is the pre-fix state.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           named_clause_closure_arg_released_at_its_bound_slot_4077) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; "
        "    Res.live := Res.live + 1; "
        "  end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class Base "
        "  public sum: int := 0; "
        "  public constructor(mk: () -> owned Res, pn: int) "
        "    using var c := mk(); "
        "    sum := c.tag + pn; "
        "  end constructor; "
        "end Base; "
        "class Derived(x: int): Base(pn := x, mk := lambda() => new Res(x)) "
        "  public method Go() Console.WriteLine(sum.ToString()); end Go; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var d := new Derived(9); "
        "      i := i + 1; "
        "    end; "
        "    using var e := new Derived(9); "
        "    e.Go(); "
        "    Console.WriteLine(Res.live.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18\n0\n"));
}

// ---------------------------------------------------------------------------
// PIN 7 — the interaction with #3562/#3742, which is where a fix to this arm
// could do real damage. The base has BOTH a §22.10 primary constructor AND a
// convenience constructor of the clause's arity that chains via `this(...)`.
// Pre-fix the resolver rejected the named clause, so `baseClauseExplicitCtor`
// reported "no callable base constructor" and
// `prePopulateBasePositionalFields` case (a) took over and stored the clause
// values into the base's PRIMARY slots BY TEXTUAL POSITION — the string into
// `n: int`. Post-fix the convenience constructor is resolved and called, case
// (a) declines (the #3742 contract: exactly one of the two evaluates these
// expressions), and `this(a, b)` fills the primary slots correctly.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           named_clause_to_convenience_ctor_of_a_primary_ctor_base_3562) {
    auto r = runAotChecked(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(b: string, a: int) "
        "    this(a, b); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(b := \"e\", a := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ne\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the ALL-POSITIONAL clause to the same explicit base constructor.
// The corpus's majority shape, and the one whose resolution this fix
// deliberately leaves to `ctorForArgs` alone (including its #3760 type-class
// rejection). Passes on both sides.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           all_positional_clause_to_explicit_base_ctor_unchanged_22_10) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string) "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(v, \"e\") "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ne\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — a NAMED clause in DECLARED ORDER. It agrees with textual order by
// construction, so `ctorForArgs`'s positional type-class check accepted it
// and it worked pre-fix. It is here to catch a fix that re-keys the loop and
// breaks a pairing which was already right.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           declared_order_named_clause_to_explicit_base_ctor_unchanged_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string) "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(a := v, b := \"e\") "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\ne\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — #3777's own shape: an ALL-POSITIONAL clause that omits a
// DEFAULTED trailing parameter of the explicit base constructor. That is the
// one short-clause form `baseClauseExplicitCtor` declares callable, and it is
// completed by `fillOmittedCtorArgs` AFTER this fix's loop runs — so a
// re-keying that changed when or how `args` is filled breaks it here.
// ---------------------------------------------------------------------------

BEZEL_TEST(inheritance_clause_explicit_ctor_named_binding_4520,
           positional_short_clause_fills_defaulted_tail_unchanged_3777) {
    auto r = runAot(
        "module M; "
        "class P "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string := \"d\") "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end P; "
        "class D(v: int) : P(v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nd\n"));
}
