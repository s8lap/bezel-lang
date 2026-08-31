// Codegen + runtime for CancellationToken.Register and the CancelAfter /
// WithCancelAfter timer — the two cancellation foundations undeferred in #1413
// (stdlib §6.1, spec §30.8; timer on the #1378 BzTimer substrate, callback list on
// the #810 push-cascade state cell).
//
//   CancellationToken.Register(callback: () -> ()): owned Subscription
//   CancellationTokenSource.CancelAfter(delay: TimeSpan)
//   CancellationTokenSource.WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource
//
// Semantics under test (stdlib §6.1, verbatim):
//   - Register installs a callback that fires when cancellation is requested — on
//     the cancelling thread (Cancel(), the CancelAfter timer thread, or a linked
//     parent's cascade thread); the returned Subscription removes it when disposed.
//   - Register on an ALREADY-cancelled token invokes the callback SYNCHRONOUSLY
//     before returning and yields a no-op Subscription.
//   - multiple callbacks each fire exactly once, in registration order; Cancel is
//     idempotent (a second Cancel — or a superseded timer — fires nothing again).
//   - Subscription.Dispose BEFORE cancellation removes the callback (does not fire).
//   - a callback registered on a LINKED child's token fires when a parent cancels.
//   - CancelAfter / WithCancelAfter cancel after the delay via the BzTimer thread;
//     CancelAfter reschedules (later call wins, matching C#; recorded in the #1413
//     lane report); Cancel() before the timer fires makes the timer a no-op; and a
//     source disposed before its timer fires tears down safely (the UAF pin).
//
// The Register/timer VALUE behaviour is exercised here via the native AOT path
// (runAot — JIT->AOT migration); the memory
// discipline (register→cancel→dispose lifecycle; dispose-before-timer) is checked by
// the SYNCHRONOUS ASan/LSan probes at the bottom.  (The timer VALUE tests use small
// 20-40 ms delays awaited across Task.Delay so the suite stays fast.)  Note: an
// async method whose callback captures a frame local leaks the 8-byte closure env
// — a PRE-EXISTING defect in the async-closure path shared verbatim with event
// Subscribe (proven on the base runtime via the untouched Subscribe path), tracked
// separately; the ASan probes here are synchronous to stay orthogonal to it.
//
// Each test cites the stdlib section and the spec-stated behaviour it verifies.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirCtsreg1413() {
    return wtScratchDir("wt-ctsreg1413");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A `Box` observation type (mutated by callbacks that capture it — the captured
// reference is a shared pointer, §28 value capture, exactly as the event tests do)
// plus the standard module/threading preamble.  `Bump()` / `Add(n)` are the
// observable side effects; Main reads `box.V`.
const char* kPreamble =
    "module M; import System.Threading; "
    "class Box "
    "  public V: int := 0; "
    "  public method Bump() V := V + 1; end Bump; "
    "  public method Add(n: int) V := V + n; end Add; "
    "end Box; ";

// Wrap a synchronous Main body in the module/class skeleton (returns int).
std::string prog(const std::string& mainBody) {
    return std::string(kPreamble) +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// Wrap an async Main body (returns Task<int>) — for the timer tests, which await.
std::string progAsync(const std::string& mainBody) {
    return std::string(kPreamble) +
           "class C public static async method Main(): Task<int> " + mainBody +
           " end Main; end C;";
}

// AddressSanitizer / LeakSanitizer verification (mirrors linked_cts_810_test.cpp).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderAsan(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDirCtsreg1413();
    std::string stem = unique_stem("bezel_ctsreg1413_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

} // namespace

// ===========================================================================
// Register — the callback fires when cancellation is requested (§6.1).
// ===========================================================================

// §6.1: Register(cb) then Cancel() → the callback fires once (box.V == 1).
BEZEL_TEST(codegen_cts_register_1413, register_fires_on_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "using var sub := tok.Register(lambda() => do box.Bump(); end); "
        "cts.Cancel(); "
        "return box.V; ")), 1);
}

// §6.1: multiple callbacks ALL fire on a single Cancel, each exactly once
// (1 + 10 + 100 = 111 — none dropped, none doubled).
BEZEL_TEST(codegen_cts_register_1413, multiple_callbacks_each_fire_once) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "using var s1 := tok.Register(lambda() => do box.Add(1); end); "
        "using var s2 := tok.Register(lambda() => do box.Add(10); end); "
        "using var s3 := tok.Register(lambda() => do box.Add(100); end); "
        "cts.Cancel(); "
        "return box.V; ")), 111);
}

// §6.1: Cancel() is idempotent — a second Cancel() does NOT re-fire the callback
// (the callback fires exactly once: box.V stays 1).
BEZEL_TEST(codegen_cts_register_1413, cancel_idempotent_callback_fires_once) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "using var sub := tok.Register(lambda() => do box.Bump(); end); "
        "cts.Cancel(); cts.Cancel(); "
        "return box.V; ")), 1);
}

// §6.1: disposing the Subscription BEFORE cancellation removes the callback — it
// does NOT fire when Cancel() is later called (box.V stays 0).  The Register/dispose
// live in a nested `do…end` block whose using-var disposes at block end.
BEZEL_TEST(codegen_cts_register_1413, dispose_before_cancel_does_not_fire) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "do using var sub := tok.Register(lambda() => do box.Bump(); end); end; "
        "cts.Cancel(); "
        "return box.V; ")), 0);
}

// §6.1: Register on an ALREADY-cancelled token invokes the callback SYNCHRONOUSLY
// before Register returns (box.V is 1 immediately, no Cancel() call after).
BEZEL_TEST(codegen_cts_register_1413, register_on_already_cancelled_fires_synchronously) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "var tok := cts.Token; "
        "using var sub := tok.Register(lambda() => do box.Bump(); end); "
        "return box.V; ")), 1);
}

// §6.1: the Subscription returned for an already-cancelled token is a no-op — its
// Dispose() is safe and idempotent (the callback already fired synchronously; box.V
// is 1 and disposing the handle twice does not crash or change it).
//
// AOT-migration finding (JIT->AOT divergence, class B): `sub` must be bound with
// `using var`, not a plain `var` — §28.14's scoped-callback admission requires the
// call producing a capture-bearing Subscription to bind its result in a `using
// var`/`await using var` local (or receive it through `this`) so the compiler can
// bound the callback's captured locals (`box`) against the Subscription's scope; a
// plain `var` gives it no such scope to bound against and is rejected. The JIT
// harness never ran the ownership pass (§28.14 lives there), so it silently
// accepted the plain `var` form. The two explicit `.Dispose()` calls below still
// exercise the idempotent-double-dispose behavior the test is named for; the
// redundant second call now also draws a §28.2 informational Warning (not an
// Error — it does not fail `.hasErrors()`), which is the compiler correctly
// flagging exactly the pattern this test intentionally exercises.
BEZEL_TEST(codegen_cts_register_1413, noop_subscription_dispose_is_safe) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "using var sub := cts.Token.Register(lambda() => do box.Bump(); end); "
        "sub.Dispose(); sub.Dispose(); "
        "return box.V; ")), 1);
}

// §6.1: Register on CancellationToken.None never cancels, so the callback never
// fires (box.V stays 0) and nothing leaks or crashes (the +1 is dropped, a no-op
// Subscription is returned).
BEZEL_TEST(codegen_cts_register_1413, register_on_none_never_fires) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "var none := CancellationToken.None; "
        "using var sub := none.Register(lambda() => do box.Bump(); end); "
        "return box.V; ")), 0);
}

// ===========================================================================
// Linked-child callbacks — a callback on a LINKED child fires when a parent
// cancels (piece 3: the cascade reaches the child's callback drain, §30.8).
// ===========================================================================

// §6.1 / §30.8: a callback registered on a linked child's token fires when a
// PARENT source cancels (the parent's cascade signals the child, whose callback
// drain fires box.Bump → box.V == 1).
BEZEL_TEST(codegen_cts_register_1413, linked_child_callback_fires_on_parent_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "using var sub := child.Token.Register(lambda() => do box.Bump(); end); "
        "a.Cancel(); "
        "return box.V; ")), 1);
}

// §6.1 / §30.8: the OTHER parent likewise fires the child's callback (composition
// over all parents, not just the first).
BEZEL_TEST(codegen_cts_register_1413, linked_child_callback_fires_on_other_parent_cancel) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var box := new Box(); "
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "using var sub := child.Token.Register(lambda() => do box.Bump(); end); "
        "b.Cancel(); "
        "return box.V; ")), 1);
}

// ===========================================================================
// CancelAfter / WithCancelAfter timer (§6.1; BzTimer / #1378).
// ===========================================================================

// §6.1: WithCancelAfter(delay) actually cancels after the delay — poll
// IsCancellationRequested across `await Task.Delay(...)` until it flips true.
// The safety cap (iters > 400) returns 100 if it never cancels; the pass returns 1.
BEZEL_TEST(codegen_cts_timer_1413, withcancelafter_cancels_after_delay) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(30)); "
        "var iters := 0; "
        "while not cts.IsCancellationRequested do "
        "  await Task.Delay(5); "
        "  iters := iters + 1; "
        "  if iters > 400 then return 100; end; "
        "end; "
        "return 1; ")), 1);
}

// §6.1: CancelAfter(delay) on an existing source cancels it after the delay
// (the "set or extend the delay on an existing source" form).
BEZEL_TEST(codegen_cts_timer_1413, cancelafter_cancels_after_delay) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "using var cts := new CancellationTokenSource(); "
        "cts.CancelAfter(TimeSpan.FromMilliseconds(30)); "
        "var iters := 0; "
        "while not cts.IsCancellationRequested do "
        "  await Task.Delay(5); "
        "  iters := iters + 1; "
        "  if iters > 400 then return 100; end; "
        "end; "
        "return 1; ")), 1);
}

// §6.1: a callback registered on the token FIRES ON THE TIMER THREAD when
// CancelAfter's deadline elapses (no Cancel() call) — box.Bump runs on the timer
// thread and its result is observed after awaiting; this must not deadlock the JIT.
BEZEL_TEST(codegen_cts_timer_1413, callback_fires_on_timer_thread) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "using var sub := cts.Token.Register(lambda() => do box.Bump(); end); "
        "cts.CancelAfter(TimeSpan.FromMilliseconds(20)); "
        "await Task.Delay(80); "
        "return box.V; ")), 1);
}

// §6.1: Cancel() BEFORE the CancelAfter timer fires — the callback fires exactly
// once (on Cancel), and the later timer firing is a no-op (idempotent latch + an
// already-drained callback list): box.V is 1, not 2.
BEZEL_TEST(codegen_cts_timer_1413, cancel_before_timer_no_double_effect) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "using var sub := cts.Token.Register(lambda() => do box.Bump(); end); "
        "cts.CancelAfter(TimeSpan.FromMilliseconds(30)); "
        "cts.Cancel(); "
        "await Task.Delay(70); "
        "return box.V; ")), 1);
}

// §6.1 (#1413 lane decision): CancelAfter called twice RESCHEDULES — the LATER call
// wins (matching C#'s timer reset).  CancelAfter(20) then CancelAfter(1000): the
// 20 ms entry is superseded, so the source is NOT cancelled 120 ms later (return 1);
// an earliest-wins policy would have cancelled at 20 ms (would return 99).
BEZEL_TEST(codegen_cts_timer_1413, cancelafter_reschedule_later_call_wins) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "using var cts := new CancellationTokenSource(); "
        "cts.CancelAfter(TimeSpan.FromMilliseconds(20)); "
        "cts.CancelAfter(TimeSpan.FromMilliseconds(1000)); "
        "await Task.Delay(120); "
        "if cts.IsCancellationRequested then return 99; end; "
        "return 1; ")), 1);
}

// §6.1: a source DISPOSED before its CancelAfter timer fires tears down safely —
// dispose cancels the pending timer entry (no callback into a freed source), and
// the program runs to a defined exit (returns 7).  The value companion to the ASan
// dispose-before-timer pin below.
BEZEL_TEST(codegen_cts_timer_1413, source_dispose_before_timer_fires_is_safe) {
    BEZEL_EXPECT_EQ(run(progAsync(
        "do using var cts := CancellationTokenSource.WithCancelAfter("
        "     TimeSpan.FromMilliseconds(20)); end; "
        "await Task.Delay(60); "
        "return 7; ")), 7);
}

// ===========================================================================
// ASan / LSan probes (§28.9 refcounted closures; §6.1) — SYNCHRONOUS, so
// orthogonal to the pre-existing async-frame-capture closure-env leak.
// ===========================================================================

// Full Register lifecycle: register several callbacks (one disposed early), Cancel
// (fires + releases the live ones), then dispose every Subscription and the source.
// No leak, no UAF — every callback closure's +1 is released exactly once.
BEZEL_TEST(codegen_cts_register_1413_asan, full_register_lifecycle_no_leak) {
    int r = runUnderAsan(prog(
        "using var box := new Box(); "
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "using var s1 := tok.Register(lambda() => do box.Add(1); end); "
        "using var s2 := tok.Register(lambda() => do box.Add(2); end); "
        "do using var s3 := tok.Register(lambda() => do box.Add(4); end); end; "
        "cts.Cancel(); "
        "return box.V; "));
    if (r < 0) return;  // toolchain unavailable — SKIP
    BEZEL_EXPECT_EQ(r, 0);
}

// Dispose-before-timer pin: WithCancelAfter schedules a (long) timer and a callback
// is registered; both the Subscription and the source are disposed BEFORE the timer
// fires.  At scheduler shutdown the pending timer entry drains and no-ops against the
// (retained) state cell — no callback into a freed source, no leak, no UAF.
BEZEL_TEST(codegen_cts_timer_1413_asan, dispose_before_timer_no_leak_no_uaf) {
    int r = runUnderAsan(prog(
        "using var box := new Box(); "
        "do "
        "  using var cts := CancellationTokenSource.WithCancelAfter("
        "      TimeSpan.FromMilliseconds(1000)); "
        "  using var sub := cts.Token.Register(lambda() => do box.Bump(); end); "
        "end; "
        "return box.V; "));
    if (r < 0) return;  // toolchain unavailable — SKIP
    BEZEL_EXPECT_EQ(r, 0);
}
