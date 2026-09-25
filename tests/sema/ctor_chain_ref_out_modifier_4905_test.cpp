// Regression tests for issue #4905 — §22.3 "Parameter modifiers": the
// call-site `ref`/`out` rule at a `this(...)` / `base(...)` constructor chain
// call.
//
// "The caller writes `ref` at the call site" (and `out` likewise), and only
// for a `ref`/`out` parameter; §22.5 makes a `ref`/`out` argument
// positional-only. Method calls and `new` expressions enforce the rule in
// sema; a chain call reached no such check (`checkDeferredNamedThisChain`'s
// applicability condition inspects the argument's modifier, never the
// formal's), so `this(42)` / `this(y := 42)` / `base(n)` against a `ref`
// formal reached codegen and ICEd: "internal: lowerExpr returned null for
// base-constructor argument — #1386" (MEASURED on 216b1cb8).
//
// spec §22.3 "Parameter modifiers" and "`ref` and `out` argument locations",
// §22.5 (ref/out arguments are positional-only), §12.8, v0.307.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

const char* kMain =
    "class P "
    "  public static method Main(): int "
    "    using var j := new J(3); Console.WriteLine(j.Tag); return 0; "
    "  end Main; "
    "end P;";

void expectMismatch(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("has a call-site `ref`/`out` mismatch: " + needle).c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "#1386"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "internal:"));
    BEZEL_EXPECT_FALSE(r.ran);
}

}  // namespace

// per §22.3 / §22.5 (v0.307), fix #4905 — the issue's named repro:
// `this(y := 42)` binds a `ref` formal by name.
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, named_this_chain_into_ref_formal_rejected) {
    expectMismatch(
        std::string("module M; "
                    "class J "
                    "  public readonly property Tag: string; "
                    "  public constructor(ref y: int, x: int := 0) "
                    "    y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
                    "  public constructor(s: string) this(y := 42); end constructor; "
                    "  public constructor(n: int) Tag := \"n\"; end constructor; "
                    "end J; ") + kMain,
        "parameter `y` is `ref` — pass the argument with `ref` at the call "
        "site, positionally");
}

// per §22.3 (v0.307), fix #4905 — the issue's positional repro: `this(42)`.
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, positional_this_chain_into_ref_formal_rejected) {
    expectMismatch(
        std::string("module M; "
                    "class J "
                    "  public readonly property Tag: string; "
                    "  public constructor(ref y: int, x: int := 0) "
                    "    y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
                    "  public constructor(n: int) this(42); end constructor; "
                    "end J; ") + kMain,
        "parameter `y` is `ref` — pass the argument with `ref` at the call site");
}

// per §22.3 (v0.307), fix #4905 — the `base(...)` spelling of the same call.
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, base_chain_into_ref_formal_rejected) {
    expectMismatch(
        std::string("module M; "
                    "class B "
                    "  public readonly property Tag: string; "
                    "  public constructor(ref y: int) y := y + 1; Tag := $\"B{y}\"; "
                    "  end constructor; "
                    "end B; "
                    "class J : B "
                    "  public constructor(n: int) base(n); end constructor; "
                    "end J; ") + kMain,
        "parameter `y` is `ref` — pass the argument with `ref` at the call site");
}

// per §22.3 (v0.307), fix #4905 — an `out` formal needs `out` at the chain.
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, this_chain_into_out_formal_rejected) {
    expectMismatch(
        std::string("module M; "
                    "class J "
                    "  public readonly property Tag: string; "
                    "  public constructor(out y: int, x: int) "
                    "    y := 9; Tag := $\"O{x},{y}\"; end constructor; "
                    "  public constructor(n: int) this(n, 5); end constructor; "
                    "end J; ") + kMain,
        "parameter `y` is `out` — pass the argument with `out` at the call site");
}

// per §22.3 (v0.307), fix #4905 — "only for a ref/out parameter": a
// superfluous `ref` at a chain into a plain formal is the mirror mismatch.
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, superfluous_ref_at_this_chain_rejected) {
    expectMismatch(
        std::string("module M; "
                    "class J "
                    "  public readonly property Tag: string; "
                    "  public constructor(y: int, x: int) Tag := $\"R{x},{y}\"; "
                    "  end constructor; "
                    "  public constructor(n: int) this(ref n, 5); end constructor; "
                    "end J; ") + kMain,
        "parameter `y` is not `ref`/`out` — remove the `ref` at the call site");
}

// Control (unchanged disposition): a correctly marked `ref` chain argument is
// accepted and aliases the parameter it names (`n` is 3, incremented to 4).
BEZEL_TEST(ctor_chain_ref_out_modifier_4905, control_ref_marked_chain_accepted) {
    auto r = runAot(std::string("module M; "
                                "class J "
                                "  public readonly property Tag: string; "
                                "  public constructor(ref y: int, x: int) "
                                "    y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
                                "  public constructor(n: int) this(ref n, 5); end constructor; "
                                "end J; ") + kMain);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("R5,4\n"));
}
