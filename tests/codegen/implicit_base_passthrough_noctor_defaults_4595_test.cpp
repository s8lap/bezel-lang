// Regression tests for issue #4595 (§22.5 "Optional parameters and named
// arguments" / §22.8 step 0-1 "The base or chained constructor call" /
// §22.10 "Primary constructors" / §12.8 "Constructor chaining", v0.306):
// a base reached through a PASS-THROUGH ancestor (a class declaring no
// §22.10 primary parameter list of its own) keeps its primary's positional
// fields at calloc zero even when that base declares NO explicit
// constructor at all.
//
// Repro (issue #4595, measured on 97718f8a — origin/main cafdfc52 plus the
// landed #4574 and #4575 fixes, which do NOT reach this shape):
//
//   class B(n: int := 1, s: string := "d")
//     public method Show()
//       Console.WriteLine(n.ToString());
//       Console.WriteLine(s);
//     end Show;
//   end B;
//   class D : B end D;
//   class E : D end E;
//   using var e := new E();
//   e.Show();                  // printed `0` and a blank line; `1` / `d`
//                              // is correct
//
// WHY #3792 AND #4574 BOTH MISS IT. `prePopulateBasePositionalFields`
// (LlvmBackend_Exprs2.cpp) is the only mechanism that materializes a §22.10
// positional parameter's default at a construction entry point, and its top
// gate reads `baseClassOf(entry)->hasPositionalFields` — `D`'s, not `B`'s —
// so for `new E()` it returns before ever examining `B`. That is exactly
// #4574's root cause; what makes this a SEPARATE shape is `B` declaring no
// explicit constructor, which makes `hasAnyConstructor(B)` false and so
// excludes it from the arm #4574 added to `emitImplicitBase`'s arity-0
// fallback. Control flow then reached that function's no-explicit-ctor
// fallback (`emitImportedBaseFieldInit` / `emitImplicitBase` +
// `emitLocalBaseFieldInit`), none of which materializes a positional
// parameter's default either (`emitOwnFieldInit` reads
// `dynamic_cast<ast::FieldDecl>(m->decl)`, which is null for a posParam
// field — its `decl` is the owning `TypeDecl`, per `FieldSymbol::posParam`'s
// own declaration comment, Symbol.hpp). The ONE-LEVEL spelling (#3792's
// shape) was already correct and is pinned as a control below.
//
// The fix drops `hasAnyConstructor(b)` from that arm's gate and replaces its
// `cls != mostDerived` double-materialization guard with
// `entryWalkFilledBase`, which asks the sharper question the widened arm
// needs: did the entry point's own `prePopulateBasePositionalFields` walk
// actually REACH this class? Rows 5 and 6 pin that sharper predicate.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// Row 1 — the issue's own repro: a pass-through ancestor (`D`) between the
// construction entry (`E`) and a NO-EXPLICIT-CONSTRUCTOR primary base (`B`).
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10 / §12.8, issue #4595
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           two_level_passthrough_noctor_base_fills_defaults) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "end D; "
        "class E : D "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 2 — control: the ONE-LEVEL spelling (`B` is the entry's DIRECT base)
// is fix #3792's shape and was already correct. Pins that the widened gate
// did not disturb it.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10, issue #4595 (control)
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           one_level_direct_noctor_base_still_correct) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 3 — TWO intervening pass-through ancestors (`F : E : D : B`): the fix
// must generalize past a single level, through `emitImplicitBase`'s own
// no-ctor-ancestor recursion.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10 / §12.8, issue #4595
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           three_level_passthrough_noctor_base_fills_defaults) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "end D; "
        "class E : D "
        "end E; "
        "class F : E "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    using var f := new F(); "
        "    f.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nd\n"));
}

// ---------------------------------------------------------------------------
// Row 4 — §22.5 "Evaluation timing": a NON-CONSTANT default is evaluated
// exactly once through the pass-through ancestor. A double-materialization
// bug that kept only the second value would still satisfy a bare value
// check, so `cnt` is asserted alongside `k`.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-1 / §22.10, issue #4595
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           two_level_noctor_default_evaluated_exactly_once) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int := 1, k: int := Counter.Next()) "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D : B "
        "end D; "
        "class E : D "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 k=1 cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Row 5 — the `entryWalkFilledBase` predicate, NO-CTOR spelling. The
// intermediate ancestor `D` HAS a primary parameter list of its own, so
// `prePopulateBasePositionalFields`'s walk does NOT stop at it: it reaches
// `B` and materializes `B`'s defaults itself. The widened arm must therefore
// stay OFF here, or `Counter.Next()` runs a second time. The retired
// `cls != mostDerived` guard answered "fire" for this shape.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-1 / §22.10, issue #4595
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           primary_list_ancestor_noctor_base_evaluated_exactly_once) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int := 1, k: int := Counter.Next()) "
        "  public method ShowB() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
        "  end ShowB; "
        "end B; "
        "class D(m: int := 2) : B "
        "  public method ShowD() "
        "    Console.WriteLine($\"m={m}\"); "
        "  end ShowD; "
        "end D; "
        "class E : D "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e.ShowB(); "
        "    e.ShowD(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 k=1 cnt=1\nm=2\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Row 6 — the same shape with `B` ALSO declaring an explicit constructor,
// i.e. the arm fix #4574 landed, reached through an intermediate ancestor
// that HAS a primary list. On 97718f8a this printed `k=2 cnt=2` — #4574's
// `cls != mostDerived` guard fired the materialization a second time on top
// of the one `prePopulateBasePositionalFields`'s walk had already done.
// `entryWalkFilledBase` repairs that too; this row pins it.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-1 / §22.10, issues #4595/#4574
BEZEL_TEST(implicit_base_passthrough_noctor_defaults_4595,
           primary_list_ancestor_explicit_ctor_base_evaluated_exactly_once) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int := 1, k: int := Counter.Next()) "
        "  public constructor(a: int) this(); end constructor; "
        "  public method ShowB() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
        "  end ShowB; "
        "end B; "
        "class D(m: int := 2) : B "
        "  public method ShowD() "
        "    Console.WriteLine($\"m={m}\"); "
        "  end ShowD; "
        "end D; "
        "class E : D "
        "end E; "
        "class C "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e.ShowB(); "
        "    e.ShowD(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 k=1 cnt=1\nm=2\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}
