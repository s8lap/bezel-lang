// Regression tests for issue #4584 (§22.8 step 0-2 "The base or chained
// constructor call" / §22.10 "Primary constructors" / §12.8 "Constructor
// chaining" / §22.5 "Evaluation timing", v0.306): a THREE-level explicit
// `base(...)` chain landing on a primary-plus-explicit-constructor base
// leaves that base's §22.10 positional fields at their calloc zero.
//
// Repro (issue #4584, measured on 97718f8a — origin/main cafdfc52 plus the
// landed #4574/#4575 fixes, neither of which reaches this shape):
//
//   class B(n: int := 3, s: string := "dflt")
//     public constructor(a: int) this(a, "ctor"); end constructor;
//     public method Show()
//       Console.WriteLine(n.ToString());
//       Console.WriteLine(s);
//     end Show;
//   end B;
//   class D : B   public constructor(a: int) base(a, "d"); end constructor; end D;
//   class E : D   public constructor(a: int) base(a);      end constructor; end E;
//   using var e := new E(9);
//   e.Show();                 // printed `0` and a blank line; `9` / `d`
//                             // is correct
//
// WHY #4574 AND #4575 BOTH MISS IT. `E`'s `base(a)` resolves to `D`'s own
// explicit constructor — a found, callable target, so `emitChainCall` simply
// calls `D`'s compiled body; nothing here is an `isThis` chain (#4575's fix
// is gated on `isThis`) or an arity-0 implicit `base()` (#4574's fix is
// `emitImplicitBase`'s arity-0 fallback). The values that go missing are
// `D`'s OWN `base(a, "d")` arguments: `prePopulateBasePositionalFields`
// (LlvmBackend_Exprs2.cpp) is the only thing that lowers them, it runs at
// the construction entry, and for `new E(9)` its top gate reads
// `baseClassOf(E)->hasPositionalFields` — `D`'s, which is false — so it
// returns without ever inspecting `B`. Inside `D`'s compiled body,
// `base(a, "d")` then finds no `ConstructorSymbol` of `B` for two arguments
// (`B`'s explicit constructor has arity 1; the call binds `B`'s PRIMARY
// list), and none of `emitChainCall`'s three `!target` arms matches
// `!isThis` + two arguments + `hasAnyConstructor(B)`, so the call does
// nothing at all: no diagnostic, no store, no base-of-base construction.
//
// The TWO-level twin (`new D(9)`, skipping `E`) was already correct — there
// `D` IS the construction entry and its own `base(a, "d")` is exactly what
// `prePopulateBasePositionalFields` inspects — and is pinned as a control.
//
// The fix extends fix #4575's pre-population call, which sits immediately
// before `emitChainCall` makes the real call to `target`, from `isThis` to
// also cover `!isThis` when `targetCls` declares no primary parameter list
// of its own. That second condition is what keeps the chain arguments at
// exactly ONE evaluation (row 5): it is the same question
// `entryWalkFilledBase` asks in `emitImplicitBase` — a `targetCls` with no
// primary list of its own is precisely one the entry walk's
// `baseClassOf(t)->hasPositionalFields` gate stops at, so no
// `prePopulateBasePositionalFields(targetCls, ...)` call was made anywhere
// else for this construction.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// Row 1 — the issue's own repro.
// ---------------------------------------------------------------------------
// per §22.8 step 0-2 / §22.10 / §12.8, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           three_level_chain_fills_primary_base_fields) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public constructor(a: int) this(a, \"ctor\"); end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor(a: int) base(a, \"d\"); end constructor; "
        "end D; "
        "class E : D "
        "  public constructor(a: int) base(a); end constructor; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(9); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 2 — control: the TWO-level twin (`new D(9)`) was already correct
// before this fix. Pins that the widened gate did not disturb it.
// ---------------------------------------------------------------------------
// per §22.8 step 0-2 / §22.10, issue #4584 (control)
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           two_level_chain_still_correct) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public constructor(a: int) this(a, \"ctor\"); end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor(a: int) base(a, \"d\"); end constructor; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 3 — a FOUR-level chain: the fix must generalize past a single extra
// level, because each hop's own `emitChainCall` re-triggers the same
// pre-population for ITS resolved target.
// ---------------------------------------------------------------------------
// per §22.8 step 0-2 / §22.10 / §12.8, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           four_level_chain_fills_primary_base_fields) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public constructor(a: int) this(a, \"ctor\"); end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor(a: int) base(a, \"d\"); end constructor; "
        "end D; "
        "class E : D "
        "  public constructor(a: int) base(a); end constructor; "
        "end E; "
        "class F : E "
        "  public constructor(a: int) base(a); end constructor; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    using var f := new F(9); "
        "    f.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 4 — EVERY level of the chain is pinned separately, and the middle
// level's base call passes a DIFFERENT value (`a + 1`) from the leaf's, so
// a fix that stored the leaf's argument into `B`'s slots would still fail.
// A three-level test that only reads the leaf can pass while the middle is
// still calloc zero.
// ---------------------------------------------------------------------------
// per §22.8 step 0-2 / §22.10 / §12.8, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           every_chain_level_holds_its_own_value) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public constructor(a: int) this(a, \"ctor\"); end constructor; "
        "  public method ShowB() "
        "    Console.WriteLine($\"B n={n} s={s}\"); "
        "  end ShowB; "
        "end B; "
        "class D : B "
        "  public dv: int; "
        "  public constructor(a: int) "
        "    base(a + 1, \"d\"); "
        "    dv := a * 10; "
        "  end constructor; "
        "  public method ShowD() "
        "    Console.WriteLine($\"D dv={dv}\"); "
        "  end ShowD; "
        "end D; "
        "class E : D "
        "  public ev: int; "
        "  public constructor(a: int) "
        "    base(a); "
        "    ev := a * 100; "
        "  end constructor; "
        "  public method ShowE() "
        "    Console.WriteLine($\"E ev={ev}\"); "
        "  end ShowE; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(9); "
        "    e.ShowB(); "
        "    e.ShowD(); "
        "    e.ShowE(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("B n=10 s=d\nD dv=90\nE ev=900\n"));
}

// ---------------------------------------------------------------------------
// Row 5 — §22.5 "Evaluation timing": the middle level's chain ARGUMENT is a
// side-effecting call, and must run exactly once. This is what the
// `!targetCls->hasPositionalFields` half of the gate buys; a fix that
// pre-populated unconditionally would lower `Counter.Next()` twice wherever
// the entry walk had already reached `targetCls`.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-2 / §22.10, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           chain_argument_evaluated_exactly_once) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public constructor(a: int) this(a, \"ctor\"); end constructor; "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor(a: int) base(Counter.Next(), \"d\"); "
        "  end constructor; "
        "end D; "
        "class E : D "
        "  public constructor(a: int) base(a); end constructor; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(9); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 s=d cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Row 6 — the same three-level chain onto a base with NO explicit
// constructor at all. `emitChainCall`'s `!isThis && !hasAnyConstructor`
// arm DOES fire for `D`'s own `base(a, "d")` there — it runs `B`'s field
// initializers — but it still never lowers the call's ARGUMENTS, so `B`'s
// positional slots stayed calloc zero for exactly the same reason.
// ---------------------------------------------------------------------------
// per §22.8 step 0-2 / §22.10 / §12.8, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           three_level_chain_onto_noctor_primary_base) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor(a: int) base(a, \"d\"); end constructor; "
        "end D; "
        "class E : D "
        "  public constructor(a: int) base(a); end constructor; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(9); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 7 — control for the shape the gate's `!targetCls->hasPositionalFields`
// half deliberately EXCLUDES: `targetCls` (`D`) declares a primary parameter
// list of its own, so `prePopulateBasePositionalFields`'s entry walk does
// reach it and the §22.10 inheritance-clause base call `: B(q, "d")` is
// already lowered exactly once. Pins that this fix left that path alone.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-2 / §22.10, issue #4584
BEZEL_TEST(three_level_base_chain_primary_fields_4584,
           primary_list_target_class_path_unchanged) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 3, s: string := \"dflt\") "
        "  public method ShowB() "
        "    Console.WriteLine($\"B n={n} s={s}\"); "
        "  end ShowB; "
        "end B; "
        "class D(q: int := 5) : B(q, \"d\") "
        "  public constructor(a: int, b: int) this(a); end constructor; "
        "  public method ShowD() "
        "    Console.WriteLine($\"D q={q}\"); "
        "  end ShowD; "
        "end D; "
        "class E : D "
        "  public constructor(a: int) base(a, 0); end constructor; "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(9); "
        "    e.ShowB(); "
        "    e.ShowD(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B n=9 s=d\nD q=9\n"));
}
