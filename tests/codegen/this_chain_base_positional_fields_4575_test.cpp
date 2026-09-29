// Regression tests for issue #4575 (§22.8 step 1 "The base or chained
// constructor call" / §12.8 "Constructor chaining" / §22.10 "Primary
// constructors", v0.306): a `base(...)` chain reached through a `this(...)`
// hop left the base's §22.10 primary positional fields at their calloc
// zero, with no diagnostic — silently constructing a half-built object.
//
// Repro (issue #4575, measured on cafdfc52):
//
//   class B(n: int, s: string)
//     public method Show()
//       Console.WriteLine(n.ToString());
//       Console.WriteLine(s);
//     end Show;
//   end B;
//   class D : B
//     public constructor() this(1); end constructor;
//     public constructor(k: int) base(k, "x"); end constructor;
//   end D;
//   using var d := new D();
//   d.Show();
//
// `new D()` selects `constructor()`, whose leading statement is `this(1)` —
// NOT `base(...)`. `prePopulateBasePositionalFields`'s case (b)
// (LlvmBackend_Exprs2.cpp) only ever inspected the ENTRY constructor's own
// leading statement; when that statement is `this(...)`, nothing examines
// the CHAINED-TO sibling's own `base(...)` at all. B has no explicit
// constructor of its own (a bare §22.10 primary parameter list), so nothing
// else ever writes its fields either: `Emitter::emitChainCall`'s
// `!target && !hasAnyConstructor(targetCls)` arm explicitly declines to
// evaluate `base(k, "x")`'s arguments, on the documented assumption that
// "the base's positional-param fields were already stored by
// prePopulateBasePositionalFields at the `new` site" — true only when the
// constructor holding that `base(...)` IS the entry point. Measured on
// cafdfc52 (this fix's parent): `bezelc` exits 0 with no diagnostic, and the
// program prints `0` and a blank line where `1` / `x` is correct.
//
// The fix adds the missing call: right before `emitChainCall` makes the
// REAL call to a `this(...)` hop's resolved target constructor, it now also
// pre-populates that target's own base's positional fields — using the
// SAME, already-lowered argument values about to be passed to the real
// call, so no argument expression is evaluated twice (§22.5 "Evaluation
// timing"). This generalizes to a hop chain of any depth (row 3 below),
// because each hop's own hand-off to `emitChainCall` re-triggers the same
// logic for ITS target.
//
// The ONE-HOP spelling of the same program — `new D(1)`, reaching
// `constructor(k: int) base(k, "x")` directly — was already correct (row
// 2), because that entry point's own leading statement IS the `base(...)`
// `prePopulateBasePositionalFields` inspects at the `new` site.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string runOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — the issue's own repro: a `base(...)` chain reached through a
// `this(...)` hop must construct the base with the real argument values.
// ---------------------------------------------------------------------------
// per §22.8 step 1 / §12.8 / §22.10, issue #4575
BEZEL_TEST(this_chain_base_positional_fields_4575,
           base_chain_via_this_hop_constructs_base) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() this(1); end constructor; "
        "  public constructor(k: int) base(k, \"x\"); end constructor; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\nx\n"));
}

// ---------------------------------------------------------------------------
// Row 2 — control: the ONE-HOP spelling of the same program, reaching
// `constructor(k: int) base(k, "x")` DIRECTLY as the construction entry
// point. Already correct before this fix (this is exactly what
// `prePopulateBasePositionalFields`'s existing case (b) handles); pins that
// the fix did not disturb it.
// ---------------------------------------------------------------------------
// per §22.8 step 1 / §12.8 / §22.10, issue #4575 (control)
BEZEL_TEST(this_chain_base_positional_fields_4575,
           base_chain_direct_entry_still_correct) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() this(1); end constructor; "
        "  public constructor(k: int) base(k, \"x\"); end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\nx\n"));
}

// ---------------------------------------------------------------------------
// Row 3 — a non-constant `this(...)`-hop ARGUMENT must be evaluated exactly
// once. This is the precise double-evaluation risk the fix's own design has
// to avoid: the REAL `this(Counter.Next())` call (compiled into the entry
// constructor's own body) evaluates `Counter.Next()` for real, exactly once,
// to invoke the sibling constructor — the fix must NOT evaluate it again to
// determine what value to pre-populate into B's field. It reuses the
// ALREADY-LOWERED argument value instead of re-lowering the AST expression,
// so `cnt` stays 1 and `n` reads the one value produced.
// ---------------------------------------------------------------------------
// per §22.5 "Evaluation timing" / §22.8 step 1 / §12.8, issue #4575
BEZEL_TEST(this_chain_base_positional_fields_4575,
           this_hop_argument_evaluated_exactly_once) {
    auto r = runAot(
        "module M; "
        "class Counter "
        "  public static N: int := 0; "
        "  public static method Next(): int "
        "    N := N + 1; "
        "    return N; "
        "  end Next; "
        "end Counter; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine($\"n={n} s={s} cnt={Counter.N}\"); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() this(Counter.Next()); end constructor; "
        "  public constructor(k: int) base(k, \"x\"); end constructor; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("n=1 s=x cnt=1\n"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cnt=2") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Row 4 — a THREE-hop `this(...)` chain: `constructor()` chains to
// `constructor(a: int)`, which chains to `constructor(a: int, b: int)`,
// which finally writes the `base(...)` call. Pins that the fix generalizes
// to any hop depth rather than only a single intervening hop, because each
// hop's own hand-off to `Emitter::emitChainCall` independently re-applies
// the same pre-population for ITS OWN resolved target.
// ---------------------------------------------------------------------------
// per §22.8 step 1 / §12.8 / §22.10, issue #4575
BEZEL_TEST(this_chain_base_positional_fields_4575,
           three_hop_this_chain_constructs_base) {
    auto r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class D : B "
        "  public constructor() this(3); end constructor; "
        "  public constructor(a: int) this(a, a + 4); end constructor; "
        "  public constructor(a: int, b: int) base(b, \"y\"); end constructor; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\ny\n"));
}

// ---------------------------------------------------------------------------
// Row 5 — control: a `this(...)`-hop chain that lands DIRECTLY on the
// enclosing class's own PRIMARY constructor (the #1087/#3583 shape,
// `Emitter::emitChainCall`'s `!target && isThis && currentThis_` arm) is a
// DIFFERENT code path from this fix's `target`-found arm and must be
// unaffected. Pins no regression to the pre-existing, already-correct
// primary-landing case.
// ---------------------------------------------------------------------------
// per §22.8 step 0-1 / §12.8 / §22.10, issue #4575 (control)
BEZEL_TEST(this_chain_base_positional_fields_4575,
           this_hop_to_own_primary_still_correct) {
    auto r = runAot(
        "module M; "
        "class D(x: int, y: string) "
        "  public constructor() this(9, \"z\"); end constructor; "
        "  public method Show() "
        "    Console.WriteLine(x.ToString()); "
        "    Console.WriteLine(y); "
        "  end Show; "
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
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nz\n"));
}
