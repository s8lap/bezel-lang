// fix #3852 — impl-ahead-of-spec removal: `Stopwatch.Restart()` (stdlib §8.1,
// v0.290, proposal 0123 R6(b)). §8.1 documents exactly `Start()`, `Stop()`,
// `Reset()` as Stopwatch's instance methods; `Restart` was never listed there
// and was undocumented compiler-only surface (two arms: the codegen call
// lowering in `LlvmBackend_Calls.cpp`'s `cls == "Stopwatch"` block, and the
// runtime entry `bz_stopwatch_restart` in `bezel_runtime_time.cpp`). Before
// this fix:
//
//     module M; class C public static method Main(): int
//       using var sw := new Stopwatch();
//       sw.Restart();
//       return 0;
//     end Main; end C;
//
// → compiled and ran. §8.1 now states the migration in prose: `Reset()`
// stops the stopwatch and zeroes `Elapsed`, so `sw.Reset(); sw.Start();` is
// byte-for-byte what the deleted `bz_stopwatch_restart` did. This fix adds a
// dedicated reject arm (`ExprTypecheck_Calls.cpp`, `cls == "Stopwatch"`,
// `m == "Restart"`) naming that replacement, and deletes both compiler arms.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `import System.Diagnostics;` is required by the §2.3 admission gate (#5034);
// the helper injects it so generated programs reach Stopwatch/Debug cleanly.
// The previous comment predates the gate.
std::string stopwatchProg(const std::string& mainBody) {
    return "module M;\nimport System.Diagnostics;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff at least one error diagnostic carries the §8.1 retired-Restart
// wording, and it names the replacement spelling.
bool rejectedAsRetiredRestart(const bezel::test::JitResult& r) {
    const std::string needle =
        "'Stopwatch' has no instance method 'Restart'";
    const std::string replacement = "sw.Reset(); sw.Start();";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find(replacement) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── REJECT — `sw.Restart()` on a `new Stopwatch()` instance. ────────────────
// Before the fix: compiled and ran (exit 0).
BEZEL_TEST(sema_stopwatch_restart_3852, reject_restart_on_new_instance) {
    auto r = runAot(stopwatchProg(
        "    using var sw := new Stopwatch();\n"
        "    sw.Restart();\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsRetiredRestart(r));
}

// ── REJECT — `sw.Restart()` on a `Stopwatch.StartNew()` instance. ───────────
// A second receiver-construction shape, so a fix that only guards `new
// Stopwatch()` receivers would be caught missing this one.
BEZEL_TEST(sema_stopwatch_restart_3852, reject_restart_on_startnew_instance) {
    auto r = runAot(stopwatchProg(
        "    using var sw := Stopwatch.StartNew();\n"
        "    sw.Restart();\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsRetiredRestart(r));
}

// ── ACCEPT-CONTROLS — the live risk of any new reject is silent
// over-rejection of the members that STAYED. Every surviving §8.1 instance
// method must still compile AND run.
namespace {

BEZEL_TEST(sema_stopwatch_restart_3852, accept_start_still_works) {
    auto r = runAot(stopwatchProg(
        "    using var sw := new Stopwatch();\n"
        "    sw.Start();\n"
        "    if sw.IsRunning then return 1; end;\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(sema_stopwatch_restart_3852, accept_stop_still_works) {
    auto r = runAot(stopwatchProg(
        "    using var sw := new Stopwatch();\n"
        "    sw.Start();\n"
        "    sw.Stop();\n"
        "    if sw.IsRunning then return 0; end;\n"
        "    return 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// The replacement spelling itself: `Reset(); Start();` must reproduce what
// `Restart()` used to do — the whole point of the migration this fix ships.
BEZEL_TEST(sema_stopwatch_restart_3852, accept_reset_then_start_replacement_works) {
    auto r = runAot(stopwatchProg(
        "    using var sw := new Stopwatch();\n"
        "    sw.Start();\n"
        "    sw.Stop();\n"
        "    sw.Reset();\n"
        "    sw.Start();\n"
        "    if sw.IsRunning then return 1; end;\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// `Stopwatch.StartNew()` (the static factory) must remain accepted — this
// fix's reject arm is guarded on `m == "Restart"` specifically and must not
// widen to reject `StartNew`.
BEZEL_TEST(sema_stopwatch_restart_3852, accept_startnew_still_works) {
    auto r = runAot(stopwatchProg(
        "    using var sw := Stopwatch.StartNew();\n"
        "    if sw.IsRunning then return 1; end;\n"
        "    return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

}  // namespace
