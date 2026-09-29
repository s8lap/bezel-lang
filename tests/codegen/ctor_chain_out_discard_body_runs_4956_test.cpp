// Regression test for issue #4956 — `this(out _, n)` / `base(out _, n)`
// against a sole `(out y: int, x: int)` chain-call target silently skipped
// the ENTIRE call: `emitChainCall`'s per-argument loop treated the discard's
// null `a.value` (Node.hpp `CallArgument::value`: "null when isDiscard") the
// same as the pre-existing defensive "the call cannot be made" case (fix
// #2579) and returned before emitting the call at all, so the target
// constructor's body — `y := 9; Tag := $"O{x}"` — never ran.
//
// spec §17.13 "The discard `_`": "As a positional argument with the `out`
// modifier ... `out _` does not introduce a variable; the receiving slot is
// internal to the compiler and is not observable." §22.6 "out _. The discard
// form applies to both parameter kinds" (only plain `out` is exercised here;
// `out owned` is a separate, not-yet-fixed gap — see this fix's commit).
// §12.8 "Constructor chaining": "the chained-to constructor runs to
// completion" before the chaining constructor's own body executes. v0.307.
//
// MEASURED pre-fix (this file's parent commit), with the issue's own repro:
// `Console.WriteLine(j.Tag)` printed an EMPTY line — `Tag` was never set,
// because the whole `this(out _, n)` call never ran — where §12.8 promises
// `O3`.

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

// per §17.13 / §22.6 / §12.8 (v0.307), fix #4956 — the issue's own repro:
// `this(out _, n)` must run the target ctor's body to completion.
BEZEL_TEST(ctor_chain_out_discard_body_runs_4956,
           this_out_discard_runs_target_body_issue_repro) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(out y: int, x: int) "
        "    y := 9; Tag := $\"O{x}\"; "
        "  end constructor; "
        "  public constructor(n: int) this(out _, n); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(3); Console.WriteLine(j.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "O3\n");
}

// per §17.13 / §22.6 / §12.8 (v0.307), fix #4956 — the `base(...)` spelling
// of the same chain-call position (§12.8: "A derived class's constructor may
// chain to a base constructor"). `emitChainCall` handles `this`/`base`
// uniformly (the `isThis` flag only affects target-class resolution), so this
// row pins that the fix is not accidentally scoped to the `this(...)`
// spelling alone.
BEZEL_TEST(ctor_chain_out_discard_body_runs_4956,
           base_out_discard_runs_target_body_spec_12_8) {
    expectPrints(
        "module M; "
        "class B "
        "  public readonly property Tag: string; "
        "  public constructor(out y: int, x: int) "
        "    y := 9; Tag := $\"O{x}\"; "
        "  end constructor; "
        "end B; "
        "class D: B "
        "  public constructor(n: int) base(out _, n); end constructor; "
        "end D; "
        "class P "
        "  public static method Main(): int "
        "    using var d := new D(4); Console.WriteLine(d.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "O4\n");
}
