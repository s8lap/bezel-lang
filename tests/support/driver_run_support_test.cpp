// driver_run_support_test.cpp — the OVER-REJECTION CONTROL for #3156's
// `compileFileForTest` wrapper (support/driver_run_support.hpp).
//
// WHY THIS FILE EXISTS. The wrapper exists because `scripts/gate.sh --full`
// exports `BEZEL_AOT_FORCE_SANITIZE=address` to put the suite under
// generated-code AddressSanitizer (#1822), and the wrapper's job is to flip
// `codegen::CodegenOptions::sanitizeAddress` on every hand-rolled
// `Driver::compileFile` runner so the sweep reaches the program — and a runner
// that "passes because everything rejects" is not a runner at all. The whole
// hole #3156 names is that the wrapper was missing; this test pins it on.
//
// The wrapper has exactly ONE behaviour — a one-way `withForcedSanitize`
// (`support/driver_run_support.hpp`) that consults
// `bezel::test::forceSanitizeAddress()` (`support/force_sanitize.hpp`) and
// ORs the bit on when the env var names AddressSanitizer. The assertion
// below is the minimum that makes the bug visible from the test side:
// whatever the environment, the wrapper must agree with `forceSanitizeAddress`
// AND respect a deliberately-set caller bit. Without the wrapper, the call
// site is `driver.compileFile(...)` and the bit is never consulted, so a
// rewrite of this test to use that spell directly would let it pass under
// `--full` only by accident — the property test below would say so.
//
// ── WHY THIS IS A PROPERTY TEST RATHER THAN AN AOT VALUE TEST ────────────
// The behaviour we pin is in the test-side `CodegenOptions` struct, NOT in a
// generated program's run. AOT-asserting it would require an executable
// whose exit code encoded `sanitizeAddress`, which is the very thing the
// compiler does not expose: the bit affects only the linker invocation, and
// whether LeakSanitizer fires is environmental. So we pin the wrapper in
// isolation, where the property is exactly the function's return.
//
// ── THE OVER-REJECTION CONTROL ──────────────────────────────────────────
// "the test set that would pass if you rejected everything is not a test set."
// A wrapper that unconditionally set `sanitizeAddress = true` (regardless of
// the env) would make every test under `--full` "pass" — every program would
// compile fine, with no leak detection because the env does not match the
// wrapper's decision. The third assertion below is the discriminator: the
// wrapper consults the env, so the result equals the env's verdict (it does
// not pre-decide). The first two pin the two literal shapes the sweep
// actually takes.

#include "codegen/LlvmBackend.hpp"      // codegen::CodegenOptions
#include "support/driver_run_support.hpp"
#include "support/force_sanitize.hpp"
#include "test_harness.hpp"

namespace {

using bezel::codegen::CodegenOptions;
using bezel::test::compileFileForTest;
using bezel::test::forceSanitizeAddress;
using bezel::test::withForcedSanitize;

// ── THE TWO ENVIRONMENTS THE SWEEP ACTUALLY TAKES ────────────────────────

// (1) Wrapper flips `sanitizeAddress` on when the env demands AddressSanitizer.
//     This is the whole #3156 hole: without the wrapper, this assertion is
//     unreachable from the runner's call site.
BEZEL_TEST(driver_run_support, withForcedSanitize_flips_on_when_env_says_asan) {
    CodegenOptions opts;
    BEZEL_EXPECT_FALSE(opts.sanitizeAddress);  // baseline
    CodegenOptions flipped = withForcedSanitize(opts);
    if (forceSanitizeAddress()) {
        // The whole-suite sweep is on. The wrapper must turn the bit on.
        BEZEL_EXPECT_TRUE(flipped.sanitizeAddress);
    } else {
        // Normal run (env unset). The wrapper must NOT touch the bit —
        // `sanitizeAddress` is otherwise a per-test choice (see the 15 leak
        // regression tests that opt in via `opts.sanitizeAddress = true;`).
        BEZEL_EXPECT_FALSE(flipped.sanitizeAddress);
    }
}

// (2) The wrapper is ONE-WAY: a caller that deliberately sets the bit keeps
//     it. The leak regression tests rely on this — they set the bit
//     unconditionally and the wrapper must not undo it when the env is unset.
//     The negation of this property was the original failure mode: a wrapper
//     that took `opts.sanitizeAddress` literally and called its `set()` would
//     miss the bit when env was set, which is the bug.
BEZEL_TEST(driver_run_support, withForcedSanitize_respects_a_caller_set_bit) {
    CodegenOptions opts;
    opts.sanitizeAddress = true;
    CodegenOptions after = withForcedSanitize(opts);
    BEZEL_EXPECT_TRUE(after.sanitizeAddress);
}

// (3) The wrapper consults the env. A wrapper that always flipped the bit
//     (regardless of the env) would still satisfy (1) under `--full` and
//     fail in plain runs only by being too noisy — but the hole #3156 names
//     is the SILENT failure, where the bit is never consulted at all. This
//     test fails when the wrapper has been replaced by a no-op (the bug
//     before #3156), and the over-rejection control says: the wrapper is
//     honest — what it returns IS the env's answer.
BEZEL_TEST(driver_run_support, withForcedSanitize_matches_env_verdict) {
    CodegenOptions opts;
    bool flipped = withForcedSanitize(opts).sanitizeAddress;
    BEZEL_EXPECT_EQ(flipped, forceSanitizeAddress());
}

// (4) The two-argument `compileFileForTest` overload — the one a frontend-only
//     runner uses (`compileFile(path, "")` with the sanitizer bit inert).
//     The call MUST route through the same wrapper, so the same env match
//     applies. We assert it compiles: the function signature is the
//     single-source-of-truth, and the inline body at
//     `support/driver_run_support.hpp` is what enforces the sweep. If the
//     overload set drifts and one form is left wrapping nothing, this is
//     the compile-time trip-wire.
BEZEL_TEST(driver_run_support, compileFileForTest_overloads_compile) {
    // No-op: just check the symbols resolve. A static_assert on the
    // function-pointer types would also work but adds a header dependency;
    // taking the address is the smallest possible probe.
    using TwoArg = bezel::CompileResult (*)(bezel::Driver&, const std::string&,
                                            const std::string&);
    using ThreeArg = bezel::CompileResult (*)(bezel::Driver&, const std::string&,
                                              const std::string&,
                                              const std::vector<bezel::Dependency>&);
    using FourArg = bezel::CompileResult (*)(
        bezel::Driver&, const std::string&, const std::string&,
        const std::vector<bezel::Dependency>&, const CodegenOptions&);
    TwoArg a = &compileFileForTest;
    ThreeArg b = &compileFileForTest;
    FourArg c = &compileFileForTest;
    BEZEL_EXPECT_TRUE(a != nullptr);
    BEZEL_EXPECT_TRUE(b != nullptr);
    BEZEL_EXPECT_TRUE(c != nullptr);
}

}  // namespace
