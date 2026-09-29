// Regression tests for issue 4550 — a §22.10 inheritance clause that names its
// arguments AND omits a §22.5-defaulted parameter of an EXPLICIT base
// constructor left the base subobject ENTIRELY UNCONSTRUCTED, with zero
// diagnostics (§22.5 "Optional parameters and named arguments" / §22.8 step 1 /
// §22.10 "Primary constructors", v0.306).
//
// THE ORACLE IS THE BASE'S STATE, not that the program compiled. An
// unconstructed base that nobody reads is invisible, so every row prints the
// base's own fields and asserts the exact text. `r.ran` alone would have been
// green on the pre-fix compiler for every one of them.
//
// MEASURED ON fcb4dbd5 (this fix's parent), the issue's own repro — `class
// D(v: int) : B(a := v)` over `constructor(a: int, b: string := "zz")` —
// compiled with `build/bezelc` at exit 0 with NO diagnostic and RAN to exit 0
// printing `0` and a blank line. `4` / `zz` is correct: the two values printed
// were `B`'s own declared field initializers, because `B`'s constructor NEVER
// RAN. The all-positional twin of the same program printed `4` / `zz` (that is
// #3777's own fix), so the plumbing worked and it was the NAMED short spelling
// that was dropped.
//
// WHAT §22.5 SAYS. "Every parameter that declares no default must be bound,
// positionally or by name" — so omitting a DEFAULTED parameter is exactly what
// the rule permits, at "every construction site in the language … a call, a
// `new` expression, and a record construction alike". §22.10 puts the clause
// among them: "When a class with a primary constructor has a base class, the
// base call appears in the inheritance clause … This replaces an explicit
// `base(...)` call."
//
// THE THREE MOVES, because the shape needed more than lifting a guard:
//
//   1. SEMA's candidate scan (`ExprTC::visitTopDecl`) falls back to
//      constructors with MORE formals than the clause supplies. Keyed on
//      EXACT arity it found no candidate for a short clause and published no
//      §22.5 binding at all. The widening is a FALLBACK — the exact-arity scan
//      still runs first and wins — so no clause that resolves today resolves
//      differently.
//   2. CODEGEN's `baseClauseExplicitCtor` admits a named short clause when
//      such a binding exists. #3777's `clauseAllPositional` restriction was
//      correct when written ("the caller's fill loop pairs clause argument `i`
//      with formal `i` POSITIONALLY, so it can only complete a TRAILING
//      omitted tail"), and both halves of that premise are now gone.
//   3. The fill is PER SLOT (`fillOmittedCtorSlots`). With names the omitted
//      slots need not be a trailing run — ROW 2 below leaves a HOLE at slot 0
//      — and `fillOmittedCtorArgs`, which APPENDS, cannot express that. It is
//      now a thin wrapper over the per-slot body, so "what value does an
//      omitted parameter take" still has one answer in the emitter.
//
// THE CONTROLS PIN THE NEIGHBOURS this touches: the all-positional short
// clause (#3777), the named FULL-arity clause (#4520), and — the one a lifted
// guard most easily breaks — that a non-constant §22.5 default is evaluated
// EXACTLY ONCE (the #4485 / #3562 family: the pre-population loop and the
// base-constructor call must not both materialize it).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// ROW 1 — the issue body's own repro. The clause names `a` and omits the
// defaulted `b`; `4` / `zz` is `B`'s constructor having run with the clause's
// value and the parameter's default.
//
// Pre-fix: compiled clean and printed `0` and a blank line — `B`'s declared
// field initializers, the base never constructed.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 1 / §22.10, issue #4550
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           named_short_clause_constructs_the_base_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public n: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(a: int, b: string := \"zz\") "
        "    n := a; "
        "    s := b; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(a := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=4 s=zz\n"));
}

// ---------------------------------------------------------------------------
// ROW 2 — the omitted slot is a HOLE, not a tail. The clause names only the
// SECOND parameter, so slot 0 is the one §22.5 leaves to its default. This is
// the row a trailing-append fill cannot pass: appending `1` after `3` puts the
// default in `k2` and the named value in `x`, printing `3` / `1`.
//
// Pre-fix: compiled clean and printed `0` / `0` — the base never constructed.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           named_clause_omitting_a_LEADING_defaulted_slot_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public a: int := 0; "
        "  public k: int := 0; "
        "  public constructor(x: int := 1, k2: int := 7) "
        "    a := x; "
        "    k := k2; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} k={k}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(k2 := 3) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=1 k=3\n"));
}

// ---------------------------------------------------------------------------
// ROW 3 — the supplied names OUT OF ORDER over a short clause, so the §22.5
// binding and the omitted-slot fill are exercised together. §22.5: "naming an
// argument changes which parameter it binds to, never when it runs."
//
// Pre-fix: compiled clean and printed `0` / empty / empty.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           named_short_clause_with_names_out_of_order_22_5) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public a: int := 0; "
        "  public s: string := \"\"; "
        "  public t: string := \"\"; "
        "  public constructor(x: int, y: string, z: string := \"zz\") "
        "    a := x; "
        "    s := y; "
        "    t := z; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} s={s} t={t}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(y := \"yy\", x := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=4 s=yy t=zz\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the ALL-POSITIONAL short clause, which is #3777's own fix and the
// shape whose exact-arity exemption this change lifts for names. It must be
// bit-for-bit what it was: `ctorForArgs` resolves it, no §22.5 binding is
// consulted, and the omitted tail takes its default.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           all_positional_short_clause_unchanged_3777) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public a: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(x: int, y: string := \"zz\") "
        "    a := x; "
        "    s := y; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} s={s}\"); "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=4 s=zz\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the named FULL-ARITY clause, #4520's own subject. The candidate
// scan's EXACT-arity arm still runs first and still wins, so the constructor
// published for this clause is the one #4520 published; a regression here
// would mean the widening replaced that arm instead of falling back to it.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           named_full_arity_clause_unchanged_4520) {
    auto r = runAot(
        "module M; "
        "class B "
        "  public a: int := 0; "
        "  public s: string := \"\"; "
        "  public constructor(x: int, y: string) "
        "    a := x; "
        "    s := y; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} s={s}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(y := \"e\", x := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=4 s=e\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the harm a newly-callable clause most easily causes: a NON-CONSTANT
// §22.5 default materialized TWICE, once by `prePopulateBasePositionalFields`
// and once by the base-constructor call this change now makes. That is the
// #4485 / #3562 family exactly, and the predicate that keeps them mutually
// exclusive is the one whose answer moved here (`baseClauseExplicitCtor`'s
// `.fn`, which now says "callable" for this shape and therefore sets
// `chainedPrimaryOwnsSlots`).
//
// `cnt` is asserted alongside `k`: a discarded first evaluation would still
// give `k=1`, and only the counter shows how many ran.
// ---------------------------------------------------------------------------
BEZEL_TEST(inheritance_clause_named_short_ctor_4550,
           omitted_default_evaluated_once_via_named_short_clause_22_5) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B "
        "  public a: int := 0; "
        "  public k: int := 0; "
        "  public constructor(x: int, k2: int := Counter.Next()) "
        "    a := x; "
        "    k := k2; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} k={k} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D(v: int) : B(x := v) "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("a=4 k=1 cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}
