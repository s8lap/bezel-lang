// Regression tests for issue #4574 (§22.5 "Optional parameters and named
// arguments" / §22.8 step 0-1 "The base or chained constructor call" /
// §22.10 "Primary constructors" / §12.8 "Constructor chaining", v0.306):
// the implicit `base()` chain never applies an ALL-DEFAULTED §22.10 primary
// parameter list's defaults when the base also declares an explicit
// constructor, and is reached through a PASS-THROUGH ancestor that has no
// primary parameter list of its own.
//
// Repro (issue #4574, measured on cafdfc52):
//
//   class B(n: int := 1, s: string := "d")
//     public constructor(a: int) this(); end constructor;
//     public method Show()
//       Console.WriteLine(n.ToString());
//       Console.WriteLine(s);
//     end Show;
//   end B;
//   class D : B
//   end D;
//   class E : D
//   end E;
//   using var e := new E();
//   e.Show();
//
// §22.5 makes B's all-defaulted primary list callable with no arguments, so
// the implicit `base()` §12.8 inserts for `D` (and, further up, for `E`) is
// legal — fix #4549 already checks exactly this and accepts the program.
// But nothing then MATERIALIZES those defaults: `Emitter::ctorForArgs(B, {})`
// (`emitImplicitBase`'s arity-0 fallback, LlvmBackend_Setup.cpp) returns
// null because a §22.10 primary constructor has no `ConstructorSymbol` (the
// same reason a direct `new` site routes to the primary, #3625/#3713) —
// and since `B` ALSO declares an explicit constructor
// (`hasAnyConstructor(B)`), the existing no-explicit-ctor field-init
// fallback (gated on `!hasAnyConstructor(b)`) never fires either. `B`'s
// positional fields stay at their calloc zero, silently.
//
// WHY THE PASS-THROUGH ANCESTOR (`D`) IS PART OF THE MINIMAL REPRO. `D`
// declares no primary parameter list of its own
// (`D->hasPositionalFields == false`), so
// `Emitter::prePopulateBasePositionalFields` — which DOES correctly
// materialize a direct base's positional defaults when called at a
// construction entry point — never reaches `B` when called for `E`: its own
// top gate tests `baseClassOf(E)->hasPositionalFields`, which is `D`'s (all
// false), not `B`'s. The ONE-LEVEL spelling `class D : B end D;` (`new
// D()`) was ALREADY correct before this fix, because there `B` IS `D`'s
// DIRECT base and `prePopulateBasePositionalFields` reaches it — pinned as
// a control below.
//
// The fix adds the missing arm to `emitImplicitBase`'s arity-0 fallback,
// gated on `cls != mostDerived` so it never re-materializes a default that
// `prePopulateBasePositionalFields` (or, for a `this(...)`-hop target, fix
// #4575's own call) already evaluated once, at the true construction entry
// point for the class whose base `b` is reached directly.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// Row 1 — the issue's own repro: a two-level pass-through ancestor (`D`, no
// primary list of its own) between the construction entry (`E`) and the
// base (`B`) whose all-defaulted primary list also has an explicit
// constructor.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10 / §12.8, issue #4574
BEZEL_TEST(implicit_base_passthrough_primary_defaults_4574,
           two_level_passthrough_fills_primary_defaults) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public constructor(a: int) this(); end constructor; "
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
// was already correct before this fix, because
// `prePopulateBasePositionalFields` reaches `B` directly at the `new` site.
// Pins that the fix did not disturb it.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10, issue #4574 (control)
BEZEL_TEST(implicit_base_passthrough_primary_defaults_4574,
           one_level_direct_base_still_correct) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public constructor(a: int) this(); end constructor; "
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
// Row 3 — a non-constant default must be evaluated exactly once, in BOTH
// the one-level and two-level spellings. This is the precise
// double-evaluation risk the fix's `cls != mostDerived` guard exists to
// avoid: the two-level case's default-fill loop must not ALSO run when
// `prePopulateBasePositionalFields` already ran for the SAME base at the
// construction entry (the one-level case, row 3b) — and the new arm must
// still run exactly once for the two-level case that genuinely needs it
// (row 3a). A discarded first evaluation (a double-eval bug where only the
// second value survives) would still pass a bare value check, which is why
// `cnt` is asserted alongside the value.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 0-1 / §22.10, issue #4574
BEZEL_TEST(implicit_base_passthrough_primary_defaults_4574,
           two_level_default_evaluated_exactly_once) {
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

BEZEL_TEST(implicit_base_passthrough_primary_defaults_4574,
           one_level_default_evaluated_exactly_once) {
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
        "  public method Show() "
        "    Console.WriteLine($\"n={n} k={k} cnt={Counter.N}\"); "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 k=1 cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Row 4 — a THREE-level pass-through chain (`F : E : D : B`), pinning that
// the fix's `cls != mostDerived` discriminator generalizes to arbitrary
// recursion depth through `emitImplicitBase`'s own no-ctor-ancestor walk,
// not just a single intervening level.
// ---------------------------------------------------------------------------
// per §22.5 / §22.8 step 0-1 / §22.10 / §12.8, issue #4574
BEZEL_TEST(implicit_base_passthrough_primary_defaults_4574,
           three_level_passthrough_fills_primary_defaults) {
    auto r = runAot(
        "module M; "
        "class B(n: int := 1, s: string := \"d\") "
        "  public constructor(a: int) this(); end constructor; "
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
