// Regression test for issue #4994 — `this(out var z, s)` / `base(out var m)`
// chain-call: the named `out var` recipient is never populated with the
// chained-to constructor's write.
//
// `emitChainCall`'s per-argument loop handled the `out var` modifier through
// the §22.3 alias path (`isByRefMod` is true for "out var"), but
// `emitAccessorRefArgCopyIn` returns null for a simple IdentifierExpr (only
// handles indexer slots and properties). The resulting null `v` hit the
// #1386 ICE instead of running the target ctor body.
//
// THE FIX: add an `out var` pre-creation block before the alias path,
// exactly mirroring `ensureOutVarSlots` / `ensureCtorOutVarSlots`. The
// recipient's slot is pre-created and `v` set to it, so `lvalueAddr` finds
// it on the alias path and the callee writes through the recipient's
// address directly (§22.3 copy-out on normal return is satisfied by the
// same mechanism).
//
// spec §22.4 ("`out var x` declares a new variable inline"), §22.3
// (copy-out on normal return), §12.8 (chained-to ctor runs to completion),
// v0.307.

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

// per §22.4 / §12.8 (v0.307), fix #4994 — the issue's repro:
// `this(out var z, n)` must run the target ctor's body to completion and
// populate the recipient `z` with the value the target wrote.
BEZEL_TEST(ctor_chain_out_var_recipient_4994,
           this_out_var_recipient_receives_written_value) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public y: int := 2; "
        "  public constructor(out z: int, x: int) "
        "    z := 9; Tag := $\"O{x}\"; "
        "  end constructor; "
        "  public constructor(n: int) this(out var z, n); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(3); Console.WriteLine(j.Tag); "
        "    return 0; "  // verify Tag was set correctly
        "  end Main; "
        "end P;",
        "O3\n");
}

// per §22.4 / §12.8 (v0.307), fix #4994 — the `base(...)` spelling of
// the same chain-call position (§12.8: "A derived class's constructor may
// chain to a base constructor"). `emitChainCall` handles `this`/`base`
// uniformly (the `isThis` flag only affects target-class resolution), so
// this row pins that the fix is not accidentally scoped to the `this(...)`
// spelling alone.
BEZEL_TEST(ctor_chain_out_var_recipient_4994,
           base_out_var_recipient_receives_written_value) {
    expectPrints(
        "module M; "
        "class B "
        "  public readonly property Tag: string; "
        "  public y: int := 2; "
        "  public constructor(out m: int, x: int) "
        "    m := 5; Tag := $\"O{x}\"; "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor(n: int) base(out var m, n); end constructor; "
        "end D; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(7); Console.WriteLine(d.Tag); "
        "    return 0; "  // verify Tag was set correctly
        "  end Main; "
        "end P;",
        "O7\n");
}
