// Regression tests for issue 4485
// (§22.5 "Evaluation timing" / §22.8 step 0 / §22.10, v0.306).
//
// `prePopulateBasePositionalFields`'s case (a) — the §22.10 inheritance-clause
// arm — declines to fill `baseArgVals` when the clause's arguments resolve to
// an EXPLICIT base constructor `emitImplicitBase` is about to lower and CALL
// (fix #3742): that call's own §22.8 step 0 writes the very positional slots
// this function would otherwise write, so filling `baseArgVals` here as well
// would be a second evaluation of the clause's argument expressions.
//
// What #3742 (and #3562's case-(b) repair before this issue) missed: the
// trailing §22.5 DEFAULT-FILL loop a few lines below is a SEPARATE loop, gated
// on its own condition (`chainedPrimaryOwnsSlots`, since fix #3562). Case (a)
// suppressed its argument loop on the #3742 predicate but never told
// `chainedPrimaryOwnsSlots` about it, so the default-fill loop still ran
// UNCONDITIONALLY for case (a): every slot the (empty) `baseArgVals` did not
// supply — which, with the argument loop suppressed, is EVERY base positional
// slot — had its §22.5 default materialized here, in addition to the
// evaluation that already happened inside the real constructor call
// `emitImplicitBase` makes. §22.8 step 1 admits exactly ONE evaluation of a
// constructor call; §22.5 "Evaluation timing" admits exactly one evaluation
// per omission ("The expression is re-evaluated every time a caller omits the
// parameter" — once per omission, not twice). A non-constant default is
// therefore evaluated twice and the slot keeps the SECOND value; an
// `owned`-factory default (§22.5 "Permitted default-value expressions" admits
// "factory methods that return owned instances") has its first product
// overwritten by the second store before any destructor can reach it — a
// leak, not merely a wrong value.
//
// This is the case-(a) twin of the defect fix #3562 repaired on case (b) (the
// explicit `base(...)`-chain arm) on these same lines, and is the "defect
// pre-existing... filed separately" the #3562 commit's scope note named:
// `class B(m, s) : A(Counter.Next())` read `cnt=2` bit-identically on
// `7f29b8ce` (before ANY of this family's fixes) and on `404a651d` (#3562
// landed) — unchanged by that commit's case-(b) repair, because case (b)'s
// `chainedPrimaryOwnsSlots` flag is set only inside the `base(...)`-chain arm,
// never the inheritance-clause arm.
//
// THE SHAPE THAT REACHES IT. Unlike #3742's own regression tests (which pin
// the ARGUMENT-evaluation half with a base clause whose arity matches the
// resolved explicit constructor's OWN formal count exactly, so nothing is
// left for the default-fill loop to touch), this file's shape gives the
// resolved explicit constructor's chained-to PRIMARY an extra, OMITTED,
// defaulted parameter — mirroring #3562's `omitted_default_evaluated_once_
// via_base_call` / `omitted_owned_default_not_leaked_via_base_call` rows,
// but reached through the §22.10 INHERITANCE CLAUSE (`class D(a: int) :
// B(a)`) rather than an explicit derived-constructor `base(...)` body.
//
//   class B(n: int, k: int := Counter.Next())
//     public constructor(v: int) this(v); end constructor;
//   end B;
//   class D(a: int) : B(a)
//   end D;
//
// `B` has no user-declared arity-1 constructor other than the one above, so
// `ctorForArgs(B, [a])` resolves it (arity 1, matching the clause) — NOT the
// primary (which has no `ConstructorSymbol` of its own, per
// `baseClauseExplicitCtor`'s declaration comment: "A primary /
// no-explicit-ctor base has no ConstructorSymbol"). `emitImplicitBase` will
// therefore CALL that constructor, whose body chains `this(v)` into `B`'s
// primary — §22.8 step 0 there captures `n` and materializes `k`'s default
// exactly once, per the construction sequence's own text:
//
//   "A constructor that chains via `this(...)` **skips steps 0 and 2** …
//    primary-parameter capture happens inside the chained-to primary."
//    (§22.8)
//
// MEASURED on 6c05b4cf (origin/main, before this fix — i.e. after #3562/#4484/
// #4491/#4496 but before #4485): `k=2 cnt=2` where §22.8 step 1 / §22.5
// "Evaluation timing" together admit exactly one evaluation (`k=1 cnt=1`);
// the `owned`-factory form of the same default (loop-amplified below) leaked
// the first construction's instance every iteration.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// ---------------------------------------------------------------------------
// HARM 1 — non-constant default evaluated twice, wrong VALUE and wrong
// side-effect COUNT. Both are asserted because either alone is satisfiable by
// the wrong lowering: a second evaluation that happened to return the same
// value would still pass a bare `k` check, and a discarded first evaluation
// would still pass a bare `cnt` check.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-1 / §22.10, issue #4485
BEZEL_TEST(base_clause_suppressed_default_fill_4485,
           omitted_default_evaluated_once_via_inheritance_clause) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int, k: int := Counter.Next()) "
        "  public constructor(v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D(a: int) : B(a) "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(9); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=9 k=1 cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// HARM 2 — the `owned`-factory form of the same default: the second
// evaluation's store overwrites the first instance's slot before any
// destructor can reach it, so the first instance's memory is unreachable —
// leaked. Loop-amplified to 200 constructions (CLAUDE.md "Oracles that pass
// vacuously": a one-shot LSan row can pass on both sides of the fix because
// the sole leaked allocation is still reachable from a live stack slot or
// register). `runAotChecked` compiles the GENERATED program under
// AddressSanitizer/LeakSanitizer.
// ---------------------------------------------------------------------------
// per §22.5 "Permitted default-value expressions" / §22.8 step 0-1 / §28.2,
// issue #4485
BEZEL_TEST(base_clause_suppressed_default_fill_4485,
           omitted_owned_default_not_leaked_via_inheritance_clause) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public Tag: int := 0; "
        "  public static Made: int := 0; "
        "  public static Dropped: int := 0; "
        "  destructor "
        "    Dropped := Dropped + 1; "
        "  end destructor; "
        "end Res; "
        "class Fac "
        "  public static method Make(): owned Res "
        "    Res.Made := Res.Made + 1; "
        "    var r := new Res(); "
        "    r.Tag := Res.Made; "
        "    return r; "
        "  end Make; "
        "end Fac; "
        "class B(n: int, owned r: Res := Fac.Make()) "
        "  public constructor(v: int) "
        "    this(v); "
        "  end constructor; "
        "  public method Tag(): int => r.Tag; "
        "end B; "
        "class D(a: int) : B(a) "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    var lastTag := 0; "
        "    for var i in 0..<200 do "
        "      using var d := new D(9); "
        "      lastTag := d.Tag(); "
        "    end; "
        "    Console.WriteLine($\"tag={lastTag} made={Res.Made} "
        "dropped={Res.Dropped}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // One `Res` per construction, each disposed: 200 made, 200 dropped, and
    // the last object's tag is the 200th. A second evaluation per
    // construction makes it 400 made with 200 of them never dropped —
    // exactly what LeakSanitizer, under `--sanitize=address`, reports.
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("tag=200 made=200 dropped=200\n"));
}

// ---------------------------------------------------------------------------
// CONTROL — the default must still ARRIVE at the slot once
// `chainedPrimaryOwnsSlots` suppresses this function's own default-fill loop.
// With a LITERAL default this cannot observe double evaluation (it is
// immortal and idempotent), which is exactly why the two rows above use a
// counted / owned default instead — but this row pins that suppressing the
// second (buggy) evaluation does not also suppress the one the chained-to
// primary's own step 0 legitimately performs.
// ---------------------------------------------------------------------------
// per §22.5 "Optional parameters and named arguments" / §22.8 step 0, #4485
BEZEL_TEST(base_clause_suppressed_default_fill_4485,
           default_still_arrives_via_inheritance_clause) {
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
        "class D(a: int) : B(a) "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(3); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\ndflt\n"));
}
