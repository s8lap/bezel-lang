// Regression tests for issue #4904 — §5.1 implicit widening (and nullable
// lifting) of the ARGUMENTS of an explicit `base(...)` / `this(...)`
// constructor chain.
//
// `emitChainCall` passed each lowered chain argument to the target
// constructor with its SOURCE width: an `int` argument at a `long`, `double`
// or `long?` formal reached the call as an `i32`, and the module failed IR
// verification ("Call parameter type does not match function signature!") —
// for positional chains and for named chains alike (the named path became
// reachable with the #4791 named-chain binding).
//
// spec §5.1 "Implicit conversions" (int widens to long/double; S → T → T?
// lifting), §22.6 stage 3 (an argument is applicable through an implicit
// conversion, which is then applied), §12.8 / §22.5 (the chain's arguments
// bind to the target's formals, by name where named), v0.307.
//
// Every bug row below failed to compile pre-fix (IR verification failure);
// the expected text is the value the spec's conversions give.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 / §22.5 (v0.307), fix #4904 — the issue's repro: named this(...)
// chain, int → long, int → double, decimal passed through, reordered names.
BEZEL_TEST(ctor_chain_arg_widening_4904, named_this_chain_widens_int_to_long_and_double) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: long := 1, y: double := 0.5, z: decimal := 2.5m) "
        "    Tag := $\"W{x},{y},{z}\"; end constructor; "
        "  public constructor(s: string) this(z := 7.25m, y := 3, x := s.Length); "
        "  end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(\"abcd\"); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "W4,3,7.25\n");
}

// per §5.1 / §22.5 (v0.307), fix #4904 — a single named int → double arg;
// the skipped formal keeps its default.
BEZEL_TEST(ctor_chain_arg_widening_4904, named_this_chain_single_int_to_double) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: long := 1, y: double := 0.5) "
        "    Tag := $\"W{x},{y}\"; end constructor; "
        "  public constructor(s: string) this(y := 3); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(\"abcd\"); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "W1,3\n");
}

// per §5.1 / §22.5 (v0.307), fix #4904 — a single named int → long arg.
BEZEL_TEST(ctor_chain_arg_widening_4904, named_this_chain_single_int_to_long) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: long := 1, y: double := 0.5) "
        "    Tag := $\"W{x},{y}\"; end constructor; "
        "  public constructor(s: string) this(x := 5); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(\"abcd\"); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "W5,0.5\n");
}

// per §5.1 / §12.8 (v0.307), fix #4904 — POSITIONAL base(...) and this(...)
// chains: int → long, int → double, int → decimal.
BEZEL_TEST(ctor_chain_arg_widening_4904, positional_base_and_this_chains_widen) {
    expectPrints(
        "module M; "
        "class B "
        "  public readonly property Tag: string; "
        "  public constructor(x: long, y: double, z: decimal) "
        "    Tag := $\"W{x},{y},{z}\"; end constructor; "
        "end B; "
        "class D : B "
        "  public constructor(n: int) base(n, n + 1, n + 2); end constructor; "
        "end D; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(x: long, y: double, z: decimal) "
        "    Tag := $\"J{x},{y},{z}\"; end constructor; "
        "  public constructor(n: int) this(n, n, n); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(5); Console.WriteLine(d.Tag); "
        "    using var j := new J(7); Console.WriteLine(j.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "W5,6,7\nJ7,7,7\n");
}

// per §5.1 lifting (S → T → T?) / §22.5 (v0.307), fix #4904 — named chain
// args at nullable-value formals: int → long? (widen + wrap) and int → int?.
BEZEL_TEST(ctor_chain_arg_widening_4904, named_this_chain_lifts_to_nullable_value) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(a: long?, b: int? := null) "
        "    Tag := $\"N{a ?? -1},{b ?? -1}\"; end constructor; "
        "  public constructor(t: string) this(b := t.Length, a := t.Length); "
        "  end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(\"xyz\"); Console.WriteLine(j.Tag); return 0; "
        "  end Main; "
        "end P;",
        "N3,3\n");
}

// per §5.1 lifting (v0.307), fix #4904 (the #3485 base(...) repros) — a
// positional base(7) at `int?` and at `long`.
BEZEL_TEST(ctor_chain_arg_widening_4904, positional_base_lifts_and_widens_literal) {
    expectPrints(
        "module M; "
        "class B "
        "  public constructor(u: int?, w: long) "
        "    Console.WriteLine($\"{u ?? -1},{w}\"); "
        "  end constructor; "
        "end B; "
        "class D : B "
        "  public constructor() base(7, 8); end constructor; "
        "end D; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "7,8\n");
}
