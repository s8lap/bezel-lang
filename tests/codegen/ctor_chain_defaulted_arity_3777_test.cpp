// Regression tests for #3777 (§22.8 step 1 / §12.8 / §22.5 / §22.6 / §22.10,
// v0.286): a constructor call written as a §12.8 CHAIN — `this(...)`,
// `base(...)`, or a §22.10 inheritance clause — that OMITS a §22.5-defaulted
// trailing parameter was mis-lowered.
//
// §22.8 step 1 is "The base or chained constructor call", and §22.5 says the
// value an omitted optional parameter receives IS its declared default. A
// `new` site has honoured both since #3082 (`ctorForArgs` admits an omitted
// defaulted tail) and #3202/#3213 (the §22.10 positional defaults). The chain
// sites did not: `emitChainCall` selected its target with `ctorByArity` —
// EXACT arity, no defaults — and `prePopulateBasePositionalFields` stored only
// `min(suppliedArgs, positionalFields)` slots.
//
// MEASURED on r12-cycle-1 (3b751a8), one bezelc build, six distinct shapes:
//
//   this(x) -> explicit sibling `(a: int, b: int := 3)`      "0/0"   (want 7/3)
//   base(v) -> base PRIMARY `P(n: int, s: string := "d")`    SIGSEGV (want 7/d)
//   base(v) -> explicit base ctor `(n, s: string := "d")`    SIGSEGV (want 7/d)
//   class D(v: int) : P(v), P a primary with a defaulted s   SIGSEGV (want 7/d)
//   class D(v: int) : P(v), P an EXPLICIT ctor with s := "d" SIGSEGV (want 7/d)
//   base() -> base ctor `(x: int := 5)`                      "0"     (want 5)
//
// The `this(...)` arm is the worst of them and the reason the family is filed
// together: `ctorByArity` found no arity-1 constructor, control fell through to
// the `!target` arms, the class had no §22.10 positional fields for the
// primary arm to write, and so NOTHING AT ALL WAS EMITTED. The object stayed
// calloc-zero, the program compiled clean at every severity, and it printed
// `0/0`. That is the quiet-wrong-answer outcome, not a crash — no oracle keyed
// on exit status or on a diagnostic can see it, which is why every row below
// asserts printed VALUES.
//
// ⚠ ONE ROW IS A KNOWN-GAP PIN, NOT AN APPROVAL. `named_chain_arg_binding_gap`
// records that a chain call written with NAMED arguments is still lowered in
// SOURCE order rather than in §22.5 binding order — a separate, pre-existing
// defect this fix deliberately does not widen (both chain sites keep the
// exact-arity rule for a named call for exactly that reason; issue 3791). It
// asserts the WRONG value on purpose so that fixing it fails loudly here.
//
// NOT COVERED, and filed: the IMPLICIT base chain still selects by exact arity
// (`emitImplicitBase`'s `ctorByArity(b, 0, nullptr)`), so a base whose
// constructor parameters are ALL defaulted is still never constructed when the
// derived constructor writes no `base()` at all — `base();` prints 5, omitting
// it prints 0, measured on this commit (issue 3792). Widening that call site
// changes which constructor runs for every class with a base, so it wants its
// own regression grid.

#include "support/aot_support.hpp"   // runAot — AOT harness (new tests: AOT only)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// (a) `this(...)` to an explicit SIBLING constructor across an omitted default.
// On 3b751a8 this printed `0/0` — the chain lowered to nothing at all.
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777, this_chain_to_explicit_sibling) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int := 3) "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(7, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/3\n"));
}

// Two omitted defaults, and the LAST one reads the one before it — §22.5
// "Permitted default-value expressions" allows a default to reference an
// earlier parameter, and the fill therefore has to run in parameter order with
// each earlier slot already bound. `this(4)` must produce a=4, b=6, c=12.
BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           this_chain_fills_two_defaults_in_order) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  private _c: int; "
        "  public constructor(a: int, b: int := 6, c: int := b * 2) "
        "    _a := a; "
        "    _b := b; "
        "    _c := c; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}/{_c}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(4, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4/6/12\n"));
}

// §22.5's evaluation discipline: a default with a SIDE EFFECT runs exactly
// once, when the call it fills is made. The counter proves neither the old
// double-evaluation shapes (#3742) nor a skipped evaluation.
BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           this_chain_side_effecting_default_runs_once) {
    auto r = runAot(
        "module M; "
        "class T "
        "  public static method Next(): int "
        "    Console.WriteLine(\"tick\"); "
        "    return 9; "
        "  end Next; "
        "end T; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int := T.Next()) "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(7, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("tick\n7/9\n"));
}

// A CLASS-typed default at a chain site: the omitted slot has to be a real
// constructed object, not the calloc zero — reading a member off it is the
// oracle, since a null would SIGSEGV rather than print.
BEZEL_TEST(ctor_chain_defaulted_arity_3777, this_chain_class_typed_default) {
    auto r = runAot(
        "module M; "
        "class Tag "
        "  public Name: string; "
        "  public constructor(n: string) Name := n; end constructor; "
        "end Tag; "
        "class B "
        "  private _a: int; "
        "  private _t: Tag; "
        "  public constructor(a: int, t: Tag := new Tag(\"auto\")) "
        "    _a := a; "
        "    _t := t; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_t.Name}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(2, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2/auto\n"));
}

// ---------------------------------------------------------------------------
// (b) `base(...)` into a §22.10 PRIMARY base across an omitted default.
// On 3b751a8 the string default left a null pointer and the program SIGSEGV'd;
// the `int` twin below is the same gap, non-fatally (it read back 0).
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_chain_to_primary_string_default) {
    auto r = runAot(
        "module M; "
        "class P(n: int, s: string := \"d\") "
        "  public method Show(): string => $\"{n}/{s}\"; "
        "end P; "
        "class D : P "
        "  public constructor(v: int) "
        "    base(v); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/d\n"));
}

BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_chain_to_primary_int_default) {
    auto r = runAot(
        "module M; "
        "class P(n: int, k: int := 42) "
        "  public method Show(): string => $\"{n}/{k}\"; "
        "end P; "
        "class D : P "
        "  public constructor(v: int) "
        "    base(v); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/42\n"));
}

// `base()` with NO arguments into a base primary whose parameters are ALL
// defaulted. This one produces no base arguments at all, so it reaches the
// pre-population's empty-arguments path — which is why the §22.5 fill has to
// run before that path returns.
BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_chain_no_args_all_defaulted) {
    auto r = runAot(
        "module M; "
        "class P(s: string := \"d\", k: int := 4) "
        "  public method Show(): string => $\"{s}/{k}\"; "
        "end P; "
        "class D : P "
        "  public constructor(v: int) "
        "    base(); "
        "  end constructor; "
        "  public method V(): int => 1; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("d/4\n"));
}

// ---------------------------------------------------------------------------
// (c) `base(...)` into an EXPLICIT base constructor across an omitted default.
// The base has a real ConstructorSymbol, so this arm goes through
// `emitChainCall`'s selection rather than the positional pre-population.
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_chain_to_explicit_base_ctor) {
    auto r = runAot(
        "module M; "
        "class P "
        "  private _n: int; "
        "  private _s: string; "
        "  public constructor(n: int, s: string := \"d\") "
        "    _n := n; "
        "    _s := s; "
        "  end constructor; "
        "  public method Show(): string => $\"{_n}/{_s}\"; "
        "end P; "
        "class D : P "
        "  public constructor(v: int) "
        "    base(v); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/d\n"));
}

// `base()` with no arguments into an explicit base constructor whose ONLY
// parameter is defaulted. On 3b751a8 this printed `0` — the base constructor
// was never called and the field kept its calloc zero, silently.
BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           base_chain_no_args_explicit_base_ctor_all_defaulted) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  private _x: int; "
        "  public constructor(x: int := 5) "
        "    _x := x; "
        "  end constructor; "
        "  public method X(): int => _x; "
        "end Base; "
        "class D : Base "
        "  public constructor(v: int) "
        "    base(); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.X().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// ---------------------------------------------------------------------------
// (d) The §22.10 INHERITANCE CLAUSE across an omitted default. §22.10: "When a
// class with a primary constructor has a base class, the base call appears in
// the inheritance clause" — "This replaces an explicit `base(...)` call", so it
// owes exactly the same fill.
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_clause_to_explicit_base_ctor) {
    auto r = runAot(
        "module M; "
        "class P "
        "  private _n: int; "
        "  private _s: string; "
        "  public constructor(n: int, s: string := \"d\") "
        "    _n := n; "
        "    _s := s; "
        "  end constructor; "
        "  public method Show(): string => $\"{_n}/{_s}\"; "
        "end P; "
        "class D(v: int) : P(v) "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/d\n"));
}

BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_clause_to_base_primary) {
    auto r = runAot(
        "module M; "
        "class P(n: int, s: string := \"d\") "
        "  public method Show(): string => $\"{n}/{s}\"; "
        "end P; "
        "class D(v: int) : P(v) "
        "  public method W(): int => v; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/d\n"));
}

// ---------------------------------------------------------------------------
// ANTI-OVER-WIDENING ROWS. These pin behaviour the fix must NOT change; each
// passes on 3b751a8 too, and that is the point.
// ---------------------------------------------------------------------------

// §22.6 "Defaulted-arity tie-break": "a candidate applicable **without filling
// any default** is preferred over a candidate that fills one or more". The
// chain selection must run its exact-arity pass FIRST — `this(x)` here has to
// reach `constructor(a: int)` (printing 99), not the fillable
// `constructor(a: int, b: int := 3)` (which would print 3).
BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           exact_arity_preferred_over_fill_22_6) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int := 3) "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(a: int) "
        "    _a := a; "
        "    _b := 99; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(7, true); "
        "    Console.WriteLine(v.Show()); "
        "    var w := new B(4); "
        "    Console.WriteLine(w.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/99\n4/99\n"));
}

// #3583's arm, unchanged: `this(...)` to the class's own §22.10 PRIMARY
// constructor across an omitted default. `ctorForArgs` must still return null
// here (a primary constructor is not a `ConstructorSymbol`, and the one
// explicit constructor present is the ENCLOSING one, excluded by §22.10's
// no-self-chain rule) so that control still reaches the primary arm.
BEZEL_TEST(ctor_chain_defaulted_arity_3777, this_chain_to_primary_unchanged) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"d\") "
        "  public constructor(x: int, y: bool) "
        "    this(x); "
        "  end constructor; "
        "  public method Show(): string => $\"{n}/{s}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(7, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/d\n"));
}

// A chain with NO defaults anywhere — every argument supplied, exact arity at
// both the `this(...)` and the `base(...)` link. Nothing about the widened
// admission may perturb it.
BEZEL_TEST(ctor_chain_defaulted_arity_3777, exact_arity_chain_unchanged) {
    auto r = runAot(
        "module M; "
        "class P "
        "  private _p: int; "
        "  public constructor(p: int) _p := p; end constructor; "
        "  public method P0(): int => _p; "
        "end P; "
        "class B : P "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int) "
        "    base(a + b); "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(x: int) "
        "    this(x, 5); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}/{P0()}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(2); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2/5/7\n"));
}

// The §22.10 base clause with EVERY argument supplied — the pre-#3777 shape of
// the `baseClauseExplicitCtor` callable test, which this fix retargets from the
// clause's argument count to the constructor's formal count.
BEZEL_TEST(ctor_chain_defaulted_arity_3777, base_clause_all_supplied_unchanged) {
    auto r = runAot(
        "module M; "
        "class P "
        "  private _n: int; "
        "  private _s: string; "
        "  public constructor(n: int, s: string := \"d\") "
        "    _n := n; "
        "    _s := s; "
        "  end constructor; "
        "  public method Show(): string => $\"{_n}/{_s}\"; "
        "end P; "
        "class D(v: int) : P(v, \"x\") "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var d := new D(7); "
        "    Console.WriteLine(d.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7/x\n"));
}

// ---------------------------------------------------------------------------
// THE OWNERSHIP COUPLING (§28.3 / §22.3 / §28.8). Lowering the chain and
// classifying it are one fact: while `emitChainCall` lowered NOTHING for this
// shape, ownership deliberately declined to call the position an owned sink
// (#3769's `ChainSite` carve-out) — declaring a transfer into a constructor
// that never runs would have suppressed the §28.3 auto-drop and leaked. With
// the chain actually lowered, the same carve-out becomes a double free: the
// value is transferred into `_s` AND auto-dropped by the convenience
// constructor. Measured, with only the codegen half applied:
//
//   AddressSanitizer: heap-use-after-free ... READ of size 4
//     #0 bz$M.Res$Id   #1 bz$M.Holder$Show   #2 main
//   freed by thread T0 here: ... #1 bz$M.Holder$ctor.1
//
// So the two halves land in one change. `runAotChecked` is ASan+LSan
// instrumented, and the loop amplifies both symptoms: a double free faults, a
// suppressed transfer leaks 200 objects.
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           owned_transfer_through_chain_across_default_exactly_once) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  public method Id(): int => _id; "
        "end Res; "
        "class Holder "
        "  private owned _s: Res; "
        "  private _t: int; "
        "  public constructor(owned r: Res, tag: int := 5) "
        "    _s := r; "
        "    _t := tag; "
        "  end constructor; "
        "  public constructor(owned x: Res, flag: bool) "
        "    this(owned x); "
        "  end constructor; "
        "  public method Sum(): int => _t + _s.Id(); "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var x := new Res(6); "
        "      using var b := new Holder(owned x, true); "
        "      acc := acc + b.Sum(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 * (5 + 6) — the defaulted `tag` really is 5, and `_s` is the
    // transferred `Res(6)` read after the chain returned, not a freed one.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2200\n"));
    // `runAotChecked` turns ANY ASan/LSan report into an Error diagnostic and a
    // non-zero exitCode, so this is the memory oracle for both directions: a
    // double free faults, a suppressed transfer leaks 200 `Res` objects.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same shape through `base(...)` into a base PRIMARY whose first parameter
// is `owned` and whose second is defaulted — issue 3769's own repro (b), whose
// value half its regression row deliberately did NOT assert, pointing here.
BEZEL_TEST(ctor_chain_defaulted_arity_3777,
           owned_base_chain_across_default_value_and_disposal) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  public method Id(): int => _id; "
        "end Res; "
        "class Base(owned r: Res, tag: int := 5) "
        "  private owned _s: Res := r; "
        "  private _t: int := tag; "
        "  public method Sum(): int => _t + _s.Id(); "
        "end Base; "
        "class D : Base "
        "  public constructor(owned v: Res) "
        "    base(owned v); "
        "  end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var x := new Res(6); "
        "      using var d := new D(owned x); "
        "      acc := acc + d.Sum(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2200\n"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// ⚠ KNOWN GAP — pinned so a fix for it FAILS HERE rather than passing silently.
//
// A chain call written with NAMED arguments is lowered in SOURCE order: the
// argument loop in `emitChainCall` pairs `call->args[ai]` with
// `target->params[ai]` positionally and never consults the §22.5 binding. So
// `this(b := 9, a := x)` against `constructor(a: int, b: int)` stores 9 into
// `a` and 7 into `b` and prints `9/7` where §22.5 requires `7/9`.
//
// Pre-existing (identical on 3b751a8) and a DIFFERENT family from this fix —
// the §22.5 BINDING at a chain site, not the §22.5 DEFAULTS. #3777 deliberately
// does not widen it: both chain sites keep the exact-arity rule for a named
// call, so a named short call is not newly selected into a loop that cannot
// lower it. Filed as issue 3791; when that lands, this row must be updated to
// `7/9` and the three all-positional gates it names can go away.
// ---------------------------------------------------------------------------

BEZEL_TEST(ctor_chain_defaulted_arity_3777, named_chain_arg_binding_gap) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor(a: int, b: int) "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(x: int, y: bool) "
        "    this(b := 9, a := x); "
        "  end constructor; "
        "  public method Show(): string => $\"{_a}/{_b}\"; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var v := new B(7, true); "
        "    Console.WriteLine(v.Show()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // KNOWN GAP: §22.5 requires "7/9". See the block comment above.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9/7\n"));
}
