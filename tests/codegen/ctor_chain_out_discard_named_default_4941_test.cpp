// Regression test for issue #4941 — `this(out _, z := n)` against a
// `(out y: int, x: int := 1, z: int := 2)` chain-call target.
//
// This is the issue's own repro: an `out _` discard SHARING the argument
// list with a NAMED tail argument (`z := n`) and an OMITTED defaulted middle
// parameter (`x`, left at its default). At the time #4941 was filed this
// combination still printed an empty `Tag` — the target constructor's body
// was skipped entirely — even though the narrower shapes were independently
// fixed: fix #4924 (b2ee727d, §22.3/§22.5/§22.6) made `emitChainCall`'s
// target selection honour call-site ref/out modifiers via `ctorForArgs`
// (which alone admits a named argument with an omitted defaulted formal),
// and fix #4956 (c7e77847, §22.3/§22.4/§22.6/§12.8/§17.13) made the
// `out _` discard argument itself lower a callee-typed scratch slot instead
// of hitting the `!a.value` defensive early-return that skipped emitting the
// call altogether.
//
// MEASURED at this test's parent commit (both #4924 and #4956 already
// landed on origin/main): the issue's repro already prints `O1,7,9` as
// spec'd — this file is a TEST-ONLY commit pinning that combination, which
// no prior test exercised (the #4956 test only pins a 2-parameter, purely
// positional `this(out _, n)` shape with no default and no named argument).
// FAIL-WITHOUT reproduced mechanically by disabling the #4956 discard arm
// alone (`if (a.isDiscard)` -> `if (false && a.isDiscard)` in
// `Emitter::emitChainCall`, LlvmBackend_Setup.cpp): the discard then falls
// through to the pre-#4956 `!a.value` early return and the whole chain call
// is skipped, printing an EMPTY line instead of `O1,7,9`. Restoring the
// source reproduces the passing behavior below.
//
// Spec: §22.3 (chain-call argument passing incl. `out`/`ref` modifiers),
// §22.5 ("Optional parameters and named arguments" — binding-with-defaults),
// §22.6 (chain-call target resolution via the same rules as `new`), §12.8
// ("Constructor chaining" — "the chained-to constructor runs to
// completion"), §17.13 ("The discard `_`" — `out _` does not introduce a
// variable). v0.309.
//
// Sibling shape NOT covered here: `this(out var t, ...)` (a NAMED `out var`
// chain-call recipient) still ICEs with the #1386 internal error — that is
// the deferred, separately tracked and still-open issue #4957
// ("`out var` chain-call recipient"), out of scope for #4941's root cause
// and left unfixed by this commit.

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

// per §22.3/§22.5/§22.6/§12.8/§17.13 (v0.309), issue #4941's own repro:
// `this(out _, z := n)` against `(out y, x := 1, z := 2)` must run the
// target ctor's body to completion, with `x` filled from its default and
// `z` bound by name.
BEZEL_TEST(ctor_chain_out_discard_named_default_4941,
           this_out_discard_named_tail_defaulted_middle_issue_repro) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(out y: int, x: int := 1, z: int := 2) "
        "    y := 9; Tag := $\"O{x},{z},{y}\"; "
        "  end constructor; "
        "  public constructor(n: int) this(out _, z := n); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var a := new J(7); Console.WriteLine(a.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "O1,7,9\n");
}

// Control (per §22.3/§22.5/§22.6, v0.309): the same target-selection and
// named-argument-with-defaulted-middle-parameter shape WITHOUT an `out`
// parameter at all — isolates that #4941's defect was specific to the `out
// _` discard argument, not to the named-tail/defaulted-middle chain
// resolution alone (already independently covered by #4924's own tests).
BEZEL_TEST(ctor_chain_out_discard_named_default_4941,
           control_no_out_named_tail_defaulted_middle) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(y: int, x: int := 1, z: int := 2) "
        "    Tag := $\"O{x},{z},{y}\"; "
        "  end constructor; "
        "  public constructor(n: int) this(0, z := n); end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var a := new J(7); Console.WriteLine(a.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "O1,7,0\n");
}
