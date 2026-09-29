// Regression tests for #3583 (§22.8 step 0 / §22.10 / §22.5, v0.286) and the
// codegen half of issue 3562.
//
// §22.8's construction sequence gives step 0 — "primary-constructor parameters
// … are captured into their compiler-generated private fields" — exactly ONE
// site for a class reached through a §22.10 convenience constructor:
//
//   "A constructor that chains via `this(...)` **skips steps 0 and 2**: its
//    effective sequence is the chained `this(...)` call, then its own body.
//    (Step 0 is vacuous for such a constructor anyway: only non-primary
//    constructors may chain via `this(...)` (§22.7, §22.10), and
//    primary-parameter capture happens inside the chained-to primary.)"
//
// The compiler had it TWICE, and the second copy was weaker than the first:
//
//   * `Emitter::storePrimaryCtorPositionalArgs` at the `new` site paid the
//     §22.5 binding (#3196), the §22.5 defaults local and imported
//     (#3202/#3213), the §4.7 unit rescue (#3502), the string +1 (#1605) and
//     the #597/#2367 evaluation brackets — but it ran for a call bound to a
//     CONVENIENCE constructor too, pairing argument i with primary positional
//     field i against a parameter list it does not describe;
//   * `emitChainCall`'s `this(...)`-to-primary arm paid NONE of them and gated
//     itself on `allPositional && n == posFields.size()`, so a chain call with
//     named arguments or one omitting a defaulted parameter stored NOTHING.
//
// Each defect hid the other: the blind `new`-site store covered the chain
// site's two holes, and the chain site's completeness would have made the blind
// store visibly redundant. Removing only the blind store regressed the three
// programs #3583 tabulated; completing only the chain site left the wild
// pointer. They land together.
//
// Measured on 907ce0f (before) → this tree (after), by row:
//
//   reordered_convenience_named_chain          139 SIGSEGV      → 3 / hi
//   reordered_convenience_defaulted_chain      139 SIGSEGV      → 3 / dflt
//   chain_position_primary_param_read_*        139 SIGSEGV      → compile error
//   defaulted_param_short_chain                7 / dflt         → 7 / dflt
//   named_chain_args                           3 / hi           → 3 / hi
//   base_positional_prepopulation              8 / conv / 8     → 8 / conv / 8
//
// Every row asserts printed VALUES, not exit status: the whole risk in this
// area is a fix that silences a crash by zeroing the object, which an
// exit-status oracle cannot see.

#include "support/aot_support.hpp"   // runAot — AOT harness (new tests: AOT only)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// The two shapes that CRASHED before the fix, and now produce the right values.
// A convenience constructor whose parameter list is REORDERED relative to the
// primary's: the blind position-i store put the `string` argument in the `int`
// slot and vice versa. On 907ce0f both printed a garbage integer and then died
// on SIGSEGV; the fix must produce the CORRECT values, not zeros — the
// loud→quiet trade a defensive check inside the blind store would have made.
// ---------------------------------------------------------------------------

BEZEL_TEST(primary_ctor_step0_unified_3583,
           reordered_convenience_named_chain) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(w: string, v: int) "
        "    this(s := w, n := v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(\"hi\", 3); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhi\n"));
}

BEZEL_TEST(primary_ctor_step0_unified_3583,
           reordered_convenience_defaulted_chain) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dflt\") "
        "  public constructor(w: string, v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(\"hi\", 3); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\ndflt\n"));
}

// The all-positional reordering: the convenience signature is (string, int) but
// the chain restores the primary's own order. Type-sound under the old blind
// pairing only by accident of the chain overwriting both slots afterwards.
BEZEL_TEST(primary_ctor_step0_unified_3583,
           reordered_convenience_positional_chain) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(w: string, v: int) "
        "    this(v, w); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(\"swap\", 9); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nswap\n"));
}

// ---------------------------------------------------------------------------
// The three shapes #3583 measured REGRESSING under the naive one-line
// `!ctorForArgs` narrowing — each one a consumer of the blind store that had
// never been recorded. They are the reason this had to be a unification and
// not a guard: the chain site now pays the §22.5 default (#3202) and the §22.5
// named binding (#3196), and the §22.10 inheritance-clause base call moved to
// run after the chain's step 0.
// ---------------------------------------------------------------------------

// Gap 1 — the chain omits a trailing DEFAULTED primary parameter. The chain
// arm's old exact-arity gate dropped the whole store, so this printed
// `0` / empty under the naive narrowing.
BEZEL_TEST(primary_ctor_step0_unified_3583, defaulted_param_short_chain) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dflt\") "
        "  public constructor(v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(7); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\ndflt\n"));
}

// Gap 2 — the chain is written with NAMED arguments. The chain arm's old
// `allPositional` requirement dropped the whole store.
BEZEL_TEST(primary_ctor_step0_unified_3583, named_chain_args) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: string) "
        "    this(s := w, n := v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(3, \"hi\"); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhi\n"));
}

// Gap 3 — §22.10's inheritance-clause base call. It is evaluated against the
// object's OWN positional slots, so on a convenience-bound `new` it has to run
// after the chained-to primary's step 0, not at the `new` site. The last `8`
// is the base's `p`; it read `0` under the naive narrowing.
BEZEL_TEST(primary_ctor_step0_unified_3583, base_positional_prepopulation) {
    auto r = runAot(
        "module M; "
        "class A(p: int) "
        "  public method ShowA() "
        "    Console.WriteLine(p.ToString()); "
        "  end ShowA; "
        "end A; "
        "class B(n: int, s: string): A(n) "
        "  public constructor(v: int) "
        "    this(v, \"conv\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "    ShowA(); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(4, \"prim\"); "
        "    b.Show(); "
        "    using var c := new B(8); "
        "    c.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nprim\n4\n8\nconv\n8\n"));
}

// A multi-level §22.10 chain reached through a convenience constructor:
// `Leaf(k, u): Mid(k, u)` and `Mid(m, t): Root(m)`. The base pre-population
// recurses, so all three levels must see the chained-to primary's values.
BEZEL_TEST(primary_ctor_step0_unified_3583, multilevel_base_chain) {
    auto r = runAot(
        "module M; "
        "class Root(r: int) "
        "  public method ShowR() Console.WriteLine(r.ToString()); end ShowR; "
        "end Root; "
        "class Mid(m: int, t: string): Root(m) "
        "  public method ShowM() "
        "    Console.WriteLine(m.ToString()); "
        "    Console.WriteLine(t); "
        "  end ShowM; "
        "end Mid; "
        "class Leaf(k: int, u: string): Mid(k, u) "
        "  public constructor(v: int) "
        "    this(v, \"leaf\"); "
        "  end constructor; "
        "  public method ShowL() "
        "    Console.WriteLine(k.ToString()); "
        "    Console.WriteLine(u); "
        "  end ShowL; "
        "end Leaf; "
        "class C "
        "  public static method Main(): int "
        "    using var l := new Leaf(21); "
        "    l.ShowL(); l.ShowM(); l.ShowR(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("21\nleaf\n21\nleaf\n21\n"));
}

// ---------------------------------------------------------------------------
// CONTROLS — the paths that must be bit-for-bit unaffected.
// ---------------------------------------------------------------------------

// The PRIMARY-bound `new` in a class that ALSO declares a convenience
// constructor. This is the shape a coarser repair (`!hasAnyConstructor(t)`,
// the guard the two IMPORTED arms use) was measured to zero, and the one the
// §22.8-exact `chains via this(...)` predicate keeps. Both a primary-bound
// call (`new B(3, "hi")`, which binds the 2-arg CONVENIENCE ctor here) and a
// short one (`new B(4)`, which reaches the primary and takes its default).
BEZEL_TEST(primary_ctor_step0_unified_3583, primary_bound_control) {
    auto r = runAot(
        "module M; "
        "class A(p: int) "
        "  public method ShowA() Console.WriteLine(p.ToString()); end ShowA; "
        "end A; "
        "class B(n: int, s: string := \"dflt\"): A(n) "
        "  public constructor(v: int, w: string) "
        "    this(v, w); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b1 := new B(3, \"hi\"); "
        "    b1.Show(); b1.ShowA(); "
        "    using var b2 := new B(4); "
        "    b2.Show(); b2.ShowA(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhi\n3\n4\ndflt\n4\n"));
}

// A `this(...)` chain that targets an explicit SIBLING convenience constructor
// rather than the primary. `emitChainCall` reaches its primary branch only when
// `ctorByArity` finds no sibling of the call's arity, and sema publishes the
// §22.5 primary binding under the same condition — so a mismatch between the
// two would mis-store exactly here.
BEZEL_TEST(primary_ctor_step0_unified_3583, sibling_convenience_chain) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int) "
        "    this(v, \"one\"); "
        "  end constructor; "
        "  public constructor(v: int, w: string, extra: int) "
        "    this(v + extra, w); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new B(2); "
        "    a.Show(); "
        "    using var b := new B(10, \"two\", 5); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\none\n15\ntwo\n"));
}

// A class with NO primary constructor whose convenience constructor chains via
// `this(...)`: the whole of §22.8 step 0 is vacuous for it (`hasPositionalFields`
// is false), and neither the `new`-site guard nor the chain-site store may
// change anything.
BEZEL_TEST(primary_ctor_step0_unified_3583, non_primary_class_this_chain) {
    auto r = runAot(
        "module M; "
        "class P "
        "  private _a: int; "
        "  private _b: string; "
        "  public constructor(a: int, b: string) "
        "    _a := a; _b := b; "
        "  end constructor; "
        "  public constructor(a: int) "
        "    this(a, \"z\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(_a.ToString()); "
        "    Console.WriteLine(_b); "
        "  end Show; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new P(4); "
        "    p.Show(); "
        "    using var q := new P(9, \"w\"); "
        "    q.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\nz\n9\nw\n"));
}

// The convenience constructor's own BODY still runs after the chain, and reads
// the slots the chained-to primary's step 0 wrote (§22.8: "its effective
// sequence is the chained `this(...)` call, then its own body"). `_extra` is a
// field initializer (step 2 of the chained-to constructor) overwritten by the
// chaining constructor's body — 7 + 100.
BEZEL_TEST(primary_ctor_step0_unified_3583, chaining_ctor_body_sees_step0) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string := \"dd\") "
        "  private _extra: int := 3; "
        "  public constructor(v: int) "
        "    this(v); "
        "    _extra := n + 100; "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "    Console.WriteLine(_extra.ToString()); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(7); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\ndd\n107\n"));
}

// ---------------------------------------------------------------------------
// The chaining-position read (issue 3562's repro, issue 3585's question).
//
// Removing the blind `new`-site store leaves this program reading a slot the
// chained-to primary has not written yet. There is no value it could produce:
// §22.8 puts the capture at step 0 of the chained-to primary, which runs
// strictly after the chain call's arguments are evaluated. Turning the SIGSEGV
// into a quiet read of the calloc zero would be a loud→quiet trade, so it is
// REJECTED — the outcome issues 3562 and 3585 both recommend.
// ---------------------------------------------------------------------------

BEZEL_TEST(primary_ctor_step0_unified_3583,
           chain_position_primary_param_read_rejected) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: int) "
        "    this(v, s); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(5, 9); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(
                "cannot read primary-constructor parameter `s` in the "
                "chaining position") != std::string::npos &&
            d.message.find("§22.8 step 0, §22.10") != std::string::npos)
            sawRule = true;
    }
    BEZEL_EXPECT_TRUE(sawRule);
}

// The `this.`-qualified spelling of the same read. §22.10's capture rule makes
// the parameter a compiler-generated field, so both spellings reach the same
// unwritten slot and both must be rejected.
BEZEL_TEST(primary_ctor_step0_unified_3583,
           chain_position_primary_param_read_this_spelling) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: int) "
        "    this(v, this.s); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(5, 9); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(
                "cannot read primary-constructor parameter `s` in the "
                "chaining position") != std::string::npos)
            sawRule = true;
    }
    BEZEL_EXPECT_TRUE(sawRule);
}

// The read NESTED inside a larger chain argument — the walk must not stop at
// the argument's top-level expression node.
BEZEL_TEST(primary_ctor_step0_unified_3583,
           chain_position_primary_param_read_nested) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: int) "
        "    this(v + n, \"x\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(5, 9); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool sawRule = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(
                "cannot read primary-constructor parameter `n` in the "
                "chaining position") != std::string::npos)
            sawRule = true;
    }
    BEZEL_EXPECT_TRUE(sawRule);
}

// CONTROL for the rejection: reading the convenience constructor's OWN
// PARAMETER of the same name as a primary parameter is fine — the name
// resolves to the parameter, not to the unwritten capture slot. The gate reads
// the resolved SYMBOL, not the spelling, so this must still compile and run.
BEZEL_TEST(primary_ctor_step0_unified_3583,
           chain_position_shadowing_parameter_admitted) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(n: int, s: string, extra: int) "
        "    this(n + 1 + extra, s); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new B(5, \"ok\", 2); "
        "    b.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\nok\n"));
}
