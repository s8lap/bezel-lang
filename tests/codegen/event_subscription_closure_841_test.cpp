// Codegen + runtime — event-handler closure environment released on
// subscription dispose, issue #841 (§24 / §27.5).
//
// BACKGROUND (#841): when an event handler is a CAPTURING lambda, the lambda's
// closure environment was not freed when the Subscription was disposed.
// LeakSanitizer reported the closure env + captured pointers as leaked.
//
// ROOT CAUSE: bz_subscription_dispose called bz_event_remove_token to set
// active=false, but neither function released the handler's closure pointer.
// The closure is a transferred +1 (bz_event_subscribe takes ownership with no
// separate retain), so the event source must release it exactly once on
// unsubscribe.  It never did.
//
// THE FIX (bz_subscription_dispose in bezel_runtime.cpp):
//   After the live-set erase confirms this is the first (live) dispose, scan
//   the source's handler array for the matching token, bz_closure_release its
//   closure (null-safe → non-capturing null-env handlers handled correctly),
//   null the slot (defensive), and deactivate.  The live-set guard makes a
//   second Dispose a no-op, so no double-release is possible.
//
// DOUBLE-FREE SAFETY:
//   - The live-set erase check (== 0 → return) ensures the release path runs
//     at most once per subscription.
//   - The active=false + nulled closure slot ensure the raise loop (which skips
//     inactive handlers) never touches the released closure → no UAF.
//
// TEST PLAN (per §24 / §27.5 / #841):
//   841-a  Capturing-handler repro: subscribe with capturing lambda, fire,
//          dispose sub; handler env + captured class env must be leak-free.
//          Returns 2. (expectValueAndLeakFree)
//   841-b  Non-capturing handler: no env, null closure; release must be null-
//          safe; also leak-free.  Returns 7.
//   841-c  Double-dispose idempotency: explicit Dispose() + using-var scope
//          exit both call bz_subscription_dispose; must be leak-free and no
//          crash / double-free.  Returns 3.
//   841-d  Event fired AFTER subscription disposed: handler must not be invoked
//          (the active=false tombstone is respected by the raise loop).  Box
//          starts at 0; sub disposed; Fire runs; box must still be 0.
//          Returns 0 (no UAF, handler not invoked).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir841() {
    return wtScratchDir("wt-841");
}

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// Compile `source` to an object file, link against the memoized ASan-
// instrumented runtime, and run under LSan.  Returns:
//   0  — clean (no leak, no UAF/DF)
//   1  — sanitizer flagged a problem: test failure
//  -1  — toolchain unavailable / compile failed: SKIP
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir841();
    std::string stem = unique_stem("bezel_841_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Correct value AND leak-free (full LSan check).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Correct value AND no memory-safety corruption (UAF/DF); leaks tolerated.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;
    auto dir = wtDir841();
    std::string stem = unique_stem("bezel_841_safe");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return;
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ---------------------------------------------------------------------------
// Test programs
// ---------------------------------------------------------------------------

// §24 / §27.5 / #841 REPRO: the closure captures `box` (a class instance).
// Subscribe in an inner scope → fire once → scope exit disposes the sub.
// The outer Fire (after dispose) no longer reaches the handler.
// Return box.V == 2. The closure env must be released on sub dispose.
const char* k841CapturingHandlerRepro =
    "module M; "
    "record Ev(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Button "
    "  public event Clicked: Ev; "
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
    "end Button; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var b := new Button(); "
    "  do "
    "    using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
    "    b.Fire(2); "
    "  end; "
    "  b.Fire(5); "   // sub disposed → no handler → box unchanged
    "  return box.V; "  // 2
    "end Main; end C;";

// fix #4509 (§27.5 / §28.9, v0.306): the LOOP-AMPLIFIED spelling of the repro
// above, for the leak leg only. The one-shot form is a VACUOUS leak oracle: the
// capture cell it allocates is the last one, so its pointer is routinely still
// discoverable by LeakSanitizer's conservative stack scan and the row passes
// whether or not the cell was ever freed. That is not a hypothesis — on the
// pre-#4509 compiler the one-shot form reported clean while
// `LSAN_OPTIONS=use_stacks=0` on the SAME binary reported 8 B / 1 allocation,
// and 200 iterations reported 1592 B / 199 allocations. CLAUDE.md's oracle rule
// ("ship the loop, 50-500 iterations") is exactly this case.
//
// `total == 400` proves all 200 handlers ran, so the amplification is of the
// live shape. Returns 7.
const char* k841CapturingHandlerReproLoop =
    "module M; "
    "record Ev(N: int); "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Button "
    "  public event Clicked: Ev; "
    "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
    "end Button; "
    "class C public static method Main(): int "
    "  var total := 0; "
    "  for var i in 0..<200 do "
    "    using var box := new Box(); "
    "    using var b := new Button(); "
    "    do "
    "      using var sub := b.Clicked.Subscribe(lambda(e) => do box.Add(e.N); end); "
    "      b.Fire(2); "
    "    end; "
    "    total := total + box.V; "
    "  end; "
    "  if total = 400 then return 7; end; "
    "  return 1; "
    "end Main; end C;";

// §24 / §27.5 / #841 NON-CAPTURING: a non-capturing handler has no env
// (null closure environment).  bz_closure_release must be null-safe.
// A static counter verifies the handler fired.  Returns 7.
const char* k841NonCapturingHandler =
    "module M; "
    "class Cnt public static hits: int := 0; end Cnt; "
    "class Src "
    "  public event Fired; "
    "  public method Fire() Fired.Raise(); end Fire; "
    "end Src; "
    "class C public static method Main(): int "
    "  using var src := new Src(); "
    "  do "
    "    using var sub := src.Fired.Subscribe(lambda() => do Cnt.hits := Cnt.hits + 1; end); "
    "    src.Fire(); "
    "  end; "
    "  if Cnt.hits = 1 then return 7; end; "
    "  return 0; "
    "end Main; end C;";

// §24 / §2.11 / #841 DOUBLE-DISPOSE IDEMPOTENCY: an inner `using var sub`
// already disposes at scope exit; the outer explicit Dispose() runs the same
// pointer through bz_subscription_dispose a second time.  The live-set guard
// makes the second call a safe no-op.  No leak, no crash, no double-free.
// Handler fires exactly once (at fire inside the using scope). Returns 3.
const char* k841DoubleDisposeIdempotency =
    "module M; "
    "class Acc public static sum: int := 0; end Acc; "
    "class Src "
    "  public event Ev: int; "
    "  public method Fire(n: int) Ev.Raise(n); end Fire; "
    "end Src; "
    "class C public static method Main(): int "
    "  using var src := new Src(); "
    "  var sub := src.Ev.Subscribe(lambda(v) => do Acc.sum := Acc.sum + v; end); "
    "  src.Fire(3); "
    "  sub.Dispose(); "   // first dispose: releases closure, deactivates
    "  sub.Dispose(); "   // second dispose: live-set miss → safe no-op
    "  src.Fire(10); "    // no active handlers → no-op
    "  return Acc.sum; "  // 3
    "end Main; end C;";

// §24 / §27.5 / #841 POST-DISPOSE RAISE: after a subscription is disposed,
// the handler must NOT be invoked (the active=false tombstone is respected).
// Also, accessing memory via a released closure would be a UAF — ASan detects
// that.  Box starts at 0; sub disposed; Fire runs; box must remain 0.
const char* k841PostDisposeNoHandlerInvoked =
    "module M; "
    "class Box public static V: int := 0; end Box; "
    "class Src "
    "  public event Ev; "
    "  public method Fire() Ev.Raise(); end Fire; "
    "end Src; "
    "class C public static method Main(): int "
    "  using var src := new Src(); "
    "  var sub := src.Ev.Subscribe(lambda() => do Box.V := Box.V + 1; end); "
    "  sub.Dispose(); "   // dispose before any Fire
    "  src.Fire(); "      // must not invoke the disposed handler
    "  return Box.V; "    // 0 (handler was not called)
    "end Main; end C;";

// #841 BOUND-LOCAL probe (Phase-A worry): the handler is a closure stored in a
// local `h`, then passed to Subscribe.  Per #429 a closure-typed local is
// scope-released at end of scope; the #841 patch ALSO releases it on sub
// dispose.  If codegen does NOT treat the Subscribe arg as a transfer, the same
// cell is released twice → a REAL double-free in genuine Bezel code (distinct
// from the test-vehicle artifact).  Non-capturing variant; returns 7.
const char* k841BoundLocalNonCapturing =
    "module M; "
    "class Cnt public static hits: int := 0; end Cnt; "
    "class Src "
    "  public event Fired; "
    "  public method Fire() Fired.Raise(); end Fire; "
    "end Src; "
    "class C public static method Main(): int "
    "  using var src := new Src(); "
    "  var h := lambda() => do Cnt.hits := Cnt.hits + 1; end; "
    "  do "
    "    using var sub := src.Fired.Subscribe(h); "
    "    src.Fire(); "
    "  end; "
    "  if Cnt.hits = 1 then return 7; end; "
    "  return 0; "
    "end Main; end C;";

// #841 BOUND-LOCAL capturing variant: `h` captures `box`.  Returns 4.
//
// fix #3756 (§27.4): `var h := lambda(v) => ...;` has no target type at the
// lambda's own formation site — `h`'s later use as a call argument to
// `Subscribe` does not flow backward to type the lambda literal, the same
// as §27.4's own canonical reject example `var f := lambda(x) => x * 2;`.
// `v` is annotated `int` (the type `box.Add` takes) so the fixture keeps
// exercising its actual purpose (bound-local capturing handler, no double
// free) instead of the parameter-inference question this file is not about.
const char* k841BoundLocalCapturing =
    "module M; "
    "class Box "
    "  public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Src "
    "  public event Ev: int; "
    "  public method Fire(n: int) Ev.Raise(n); end Fire; "
    "end Src; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var src := new Src(); "
    "  var h := lambda(v: int) => do box.Add(v); end; "
    "  do "
    "    using var sub := src.Ev.Subscribe(h); "
    "    src.Fire(4); "
    "  end; "
    "  return box.V; "  // 4
    "end Main; end C;";

}  // namespace

// ---------------------------------------------------------------------------
// §24 / §27.5 / #841 tests
// ---------------------------------------------------------------------------

// 841-a: the capturing-handler repro is both value-correct (returns 2) AND
// leak-free after the fix.  Before #841, the closure env + captured Box ptr
// leaked (LSan reported 40 bytes / 2 allocations).
BEZEL_TEST(codegen_event_subscription_closure_841,
           capturing_handler_env_released_on_sub_dispose_24_27_5) {
    expectValueAndLeakFree(k841CapturingHandlerRepro, 2);
    // fix #4509 (§27.5 / §28.9, v0.306): the same claim, amplified, because the
    // one-shot leg above can pass by stack-scan REACHABILITY rather than by
    // reclamation (see k841CapturingHandlerReproLoop's note). Strictly stronger
    // than what this row asserted before; the one-shot legs are kept as-is.
    BEZEL_EXPECT_EQ(run(k841CapturingHandlerReproLoop), 7);
    int lsanLoop = runUnderLeakSanitizer(k841CapturingHandlerReproLoop);
    if (lsanLoop != -1)  // toolchain unavailable: skip, as the helper does
        BEZEL_EXPECT_EQ(lsanLoop, 0);
}

// 841-b: a non-capturing handler (null env) — bz_closure_release must be
// null-safe; the subscription dispose must not crash or corrupt memory.
// Leak-free (no env to release; the closure cell is released).
BEZEL_TEST(codegen_event_subscription_closure_841,
           non_capturing_handler_null_env_release_safe_24) {
    expectValueAndLeakFree(k841NonCapturingHandler, 7);
}

// 841-c: double-dispose idempotency — the live-set guard makes the second
// bz_subscription_dispose a safe no-op; no double-release of the closure.
// Leak-free and no memory-safety error (no double-free, no UAF).
BEZEL_TEST(codegen_event_subscription_closure_841,
           double_dispose_idempotent_no_double_free_2_11) {
    expectValueAndLeakFree(k841DoubleDisposeIdempotency, 3);
}

// 841-d: after a subscription is disposed, the handler is NOT invoked by a
// subsequent Raise (active=false tombstone respected).  The disposed closure
// must not be accessed via the raise loop (no UAF).  Returns 0.
BEZEL_TEST(codegen_event_subscription_closure_841,
           post_dispose_raise_handler_not_invoked_no_uaf_24) {
    // Value + no-corruption check: the program returns 0 (handler not called)
    // and ASan detects no use-after-free from reading the disposed closure slot.
    expectValueAndNoCorruption(k841PostDisposeNoHandlerInvoked, 0);
}

// 841-e: bound-local non-capturing handler — probe for a REAL double-free
// (scope-release of the closure local + #841 runtime-release on sub dispose).
BEZEL_TEST(codegen_event_subscription_closure_841,
           bound_local_noncapturing_handler_no_double_free) {
    expectValueAndNoCorruption(k841BoundLocalNonCapturing, 7);
}

// 841-f: bound-local capturing handler — same probe with a captured class ref.
BEZEL_TEST(codegen_event_subscription_closure_841,
           bound_local_capturing_handler_no_double_free) {
    expectValueAndNoCorruption(k841BoundLocalCapturing, 4);
}
