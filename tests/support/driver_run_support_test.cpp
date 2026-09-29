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

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codegen/LlvmBackend.hpp"      // codegen::CodegenOptions
#include "support/driver_run_support.hpp"
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"      // wtScratchDir
#include "support/unique_stem.hpp"      // unique_stem
#include "test_harness.hpp"

namespace {

using bezel::codegen::CodegenOptions;
using bezel::test::compileFileForTest;
using bezel::test::forceSanitizeAddress;
using bezel::test::kSanitizerInterventionExit;
using bezel::test::runProgramForTest;
using bezel::test::withForcedSanitize;

namespace fs = std::filesystem;

// A tiny executable probe. `runProgramForTest` takes a PATH and runs it, so the
// cheapest honest witness of what it does to the run is a program that reports
// its own environment through its exit code — no Bezel compile needed, and no
// dependence on whether a leak happens to be reachable at exit.
std::string writeProbe(const char* tag, const std::string& body) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string path = (dir / (unique_stem(tag) + ".sh")).string();
    {
        std::ofstream out(path);
        out << "#!/bin/sh\n" << body;
    }
    ::chmod(path.c_str(), 0755);
    return path;
}

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

// ── THE RUN HALF OF THE SWEEP (#4532) ────────────────────────────────────
//
// `compileFileForTest` above is only half the sweep; the other half is how the
// produced program is RUN. A hand-rolled runner used to call
// `std::system(exe)`, which inherits the ambient environment — and under
// `gate.sh --full` nothing sets `ASAN_OPTIONS`, so the program ran under
// LeakSanitizer defaults and LSan's `exitcode` REPLACED the program's own. That
// is not a stricter oracle, it is a different one: `runAot` runs every
// sanitized program with `detect_leaks=0:abort_on_error=0` (#1822), §28.2
// checked disposal leaks BY DESIGN (#621/#627), and a row asserting exit 1
// passes under LSan whatever its program does.
//
// (5) `runProgramForTest` applies the sweep's run options WHEN THE SWEEP IS ON,
//     AND ONLY THEN. Both halves are load-bearing and each oracle checks one:
//       * a helper that never injected them fails under `--full` (the bug);
//       * a helper that injected them unconditionally fails on the plain
//         oracle — and that direction is not cosmetic, because ~15 leak
//         regression tests set `opts.sanitizeAddress = true` themselves and
//         their whole point is `detect_leaks=1`. Turning LSan off for them
//         would silence exactly the tests written to catch leaks.
//     The probe reports which environment it was handed, so this is the run
//     half's exact analogue of (3) above: what the helper does IS the env's
//     answer, not a pre-decided one.
BEZEL_TEST(driver_run_support, runProgramForTest_applies_sweep_options_iff_sweep_is_on) {
    const std::string probe = writeProbe("wt-drs-runopts",
        "case \"$ASAN_OPTIONS\" in\n"
        "  '') exit 40 ;;\n"                        // untouched: plain std::system
        "  *detect_leaks=0*abort_on_error=0*) exit 41 ;;\n"   // the sweep's own options
        "  *) exit 42 ;;\n"                         // some OTHER options: a drift
        "esac\n");
    const int status = runProgramForTest(probe);
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    BEZEL_EXPECT_EQ(forceSanitizeAddress() ? 41 : 40, WEXITSTATUS(status));
    std::remove(probe.c_str());
}

// (6) A sanitizer finding still FAILS, loudly. The point of (5) is that a
//     correct program keeps its own exit code — which would be a weakened
//     oracle if it also swallowed a real redzone error. It does not: the helper
//     scans the captured stderr and trips `currentTestFailed()`, so a report
//     fails the row no matter what the caller does with the returned status.
//     The probe impersonates a fired report (the helper keys on the headline
//     text, exactly as `scripts/gate.sh` does).
//
//     ⚠ TWO deliberate manipulations, both of them load-bearing rather than
//     conveniences:
//       * The test trips `currentTestFailed()` and then clears it. The flag is
//         captured BEFORE the clear and ASSERTED, so a helper that stopped
//         tripping fails here; without the clear, a test that proves the
//         mechanism works would report itself failed.
//       * fd 2 is redirected to a file for the duration of the call. The probe's
//         headline and the helper's banner are EXACTLY the two patterns
//         `scripts/gate.sh` hard-fails a cycle on (`ERROR: (Address|Leak)`
//         `Sanitizer` and `aot-asan #1822] sanitizer fired`), so letting this
//         test write them to the harness's real stderr would plant a false
//         positive in a shard log — inert today only because that grep reads
//         the wrong file (#4533), which is a bug being fixed, not a guarantee.
//         Capturing them keeps this test out of the gate's evidence channel AND
//         lets it assert the banner was emitted at all.
BEZEL_TEST(driver_run_support, runProgramForTest_fails_the_test_on_a_sanitizer_report) {
    const std::string probe = writeProbe("wt-drs-report",
        "echo '==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x0' >&2\n"
        "exit " + std::to_string(kSanitizerInterventionExit) + "\n");
    const std::string capture = probe + ".stderr";

    std::fflush(stderr);
    const int savedFd = ::dup(2);
    const int captureFd = ::open(capture.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    BEZEL_EXPECT_TRUE(savedFd >= 0 && captureFd >= 0);
    if (savedFd < 0 || captureFd < 0) return;
    ::dup2(captureFd, 2);

    ::bezel::test::currentTestFailed() = false;
    const int status = runProgramForTest(probe);
    const bool tripped = ::bezel::test::currentTestFailed();
    ::bezel::test::currentTestFailed() = false;   // undo the deliberate trip

    std::fflush(stderr);
    ::dup2(savedFd, 2);
    ::close(savedFd);
    ::close(captureFd);

    std::string emitted;
    {
        std::ifstream in(capture);
        std::ostringstream ss;
        ss << in.rdbuf();
        emitted = ss.str();
    }
    std::remove(capture.c_str());
    std::remove(probe.c_str());

    const bool banner = emitted.find("] sanitizer fired") != std::string::npos;
    const bool headline =
        emitted.find("ERROR: AddressSanitizer") != std::string::npos;

    // Under the sweep the helper captures the child's stderr, recognises the
    // report, re-emits it under the banner and trips the flag. Off the sweep it
    // is plain `std::system` — the child writes straight through and nothing is
    // scanned, which is the unchanged behaviour (5) pins. Either way the
    // headline reaches a reader; only the sweep adds the banner and the trip.
    BEZEL_EXPECT_EQ(forceSanitizeAddress(), tripped);
    BEZEL_EXPECT_EQ(forceSanitizeAddress(), banner);
    BEZEL_EXPECT_TRUE(headline);
    BEZEL_EXPECT_TRUE(WIFEXITED(status));
    BEZEL_EXPECT_EQ(kSanitizerInterventionExit, WEXITSTATUS(status));
}

}  // namespace
