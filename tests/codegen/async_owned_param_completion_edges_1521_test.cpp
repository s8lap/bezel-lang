// Codegen/ownership — issue #1521 (§30.1 v0.179 "`owned` parameters transfer
// into the frame"; §28.3 "owned parameters" auto-drop; §29.8 "Iterators with
// `owned` parameters" for the shared mechanism):
//
//   "An async method's `owned` parameters transfer into the state-machine
//   frame at call time, exactly as an iterator's do (§29.8). The body may
//   move them out under the ordinary rules ... any `owned` parameter never
//   moved out is disposed by the frame when the task reaches its terminal
//   state — on every completion path (success, fault, and cancellation),
//   after the body's `using var` cleanups and `finally` clauses, in reverse
//   declaration order (§28.4's ordering). ... Disposing a not-yet-settled
//   task does not cut this short: the underlying operation runs to its
//   terminal state (§30.5), and the frame's parameters are disposed then."
//
// Pre-fix, a plain (non-iterator) `async method M(owned c: T): Task` that
// never explicitly discharged `c` HARD-ERRORED at compile time with "owned
// value in `c` would be dropped here without disposal (§28.3)" — the §28.3
// normal-exit-drop reject that governs a SYNCHRONOUS method's owned
// parameter, which does not apply to a state-machine method (its parameter
// lives in the heap frame, not a synchronous call frame the reject's
// reasoning assumes). Ownership.cpp already carried this exact carve-out for
// iterator methods (`sawYield_`, #945/#973b, flagging `ps->iteratorAutoDispose`
// for codegen to auto-dispose at iterator-Dispose) but gated it OFF whenever
// `isAsync_` was true — even for a plain async method with no `yield` at all.
//
// The fix (Ownership.cpp, LlvmBackend_Async.cpp / LlvmBackend_impl.hpp /
// LlvmBackend_Exprs2.cpp / LlvmBackend_Calls2.cpp):
//   - Ownership.cpp: widen the `sawYield_` gate to `sawYield_ || isAsync_` so
//     a plain async method's never-discharged `owned` parameter is flagged
//     `iteratorAutoDispose` instead of hard-rejected — same criterion
//     (`everDischarged_.count(sym) == 0`: never discharged on ANY path) as
//     the existing iterator case; a param discharged on SOME path (but not
//     all) is unaffected and stays a hard reject (§973a).
//   - A related gap the SAME gate exposed: the dataflow's ordinary exception-
//     path-discharge assumption ("a value that reaches exit only via a throw
//     ... does not trip the undischarged check") drops a throw-only-exit
//     symbol from `endState` entirely — correct for a SYNCHRONOUS body (where
//     codegen registers the owned local/param on the ordinary scope-unwind-
//     disposal stack) but NOT for a state-machine method's `owned` parameter,
//     whose only disposal mechanism is the frame's terminal-state auto-
//     dispose. Fixed by a supplemental parameter scan (independent of
//     `endState` membership) alongside the existing #1257 addendum for
//     nested-scope owned LOCALS — this incidentally also closes the same
//     latent gap for a THROW-ONLY sync/async ITERATOR body (tested below;
//     same code path, same gate).
//   - LlvmBackend_Async.cpp: a new `emitAsyncOwnedParamAutoDispose(self)`
//     helper (mirroring the sync iterator's `disposeOwnedParams` and the
//     async iterator's `emitFreeAndReturn` dispose loop) walks the plain
//     async method's frame captures and disposes every parameter flagged
//     `iteratorAutoDispose`, in reverse declaration order, null-guarded.
//     Wired into BOTH terminal edges of `emitAsyncMethod`: the SUCCESS edge
//     (`emitAsyncComplete`, covering every `return`/fall-off) and the
//     FAULT/CANCELLATION edge (the `async.fault` boundary — the runtime
//     models cancellation as an `OperationCanceledException` fault, so no
//     third edge is needed). A new `asyncOwnedParamFrame_` member publishes
//     the current frame for both edges to read; it is explicitly saved,
//     nulled across, and restored around the two pre-existing nested-
//     emission hazards (an async lambda built mid-emission of an outer async
//     method, LlvmBackend_Exprs2.cpp; a lazily-built generic specialization
//     built mid-emission of an outer state machine, LlvmBackend_Calls2.cpp)
//     so a nested emission never GEPs the outer frame's capture-field indices
//     into a differently-laid-out inner frame.
//
// Verification uses the `Cell` static-counter idiom (ctor ++, dtor --,
// established by tests/codegen/async_owned_task_result_1127_test.cpp): a
// leak leaves Cell.n > 0; a double-free/double-dispose underflows it or
// crashes. `Cell.n == 0` after the parameter's owning frame reaches its
// terminal state is the dispose-exactly-once assertion for every case below.
// Independently reproduced+verified via build/bezelc AOT + valgrind
// (--leak-check=full): every scenario below (as a standalone native
// executable) reports "All heap blocks were freed -- no leaks are possible"
// and "0 errors from 0 contexts". v0.179 (per issue #1521).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
// #1521 lives ENTIRELY in the ownership pass (the §28.3 reject + the §30.1
// iteratorAutoDispose flag) and the async completion-edge codegen
// (emitAsyncOwnedParamAutoDispose) that reads that flag. The shared `runJit`
// helper (tests/support/codegen_support.hpp) runs sema but NOT
// `ownership::checkOwnership` — by design it is "surface-only ... emitted
// objects are byte-identical" — so it can exercise NEITHER half of this fix:
//   - the iteratorAutoDispose flag stays unset → emitAsyncOwnedParamAutoDispose
//     is a no-op → the `owned` param LEAKS (Cell.n stays 1) on every completion
//     edge; and
//   - the §28.3 reject never fires → a conditionally-discharged param "runs"
//     instead of being rejected.
// This full-pipeline runner adds the ownership pass before codegen, mirroring
// the real driver (src/driver/Driver.cpp) and the established #945 precedent
// (iterator_owned_param_945_test.cpp's run(), which exists for the identical
// reason — that test's header documents the same runJit-skips-ownership gap).
// Without it these tests mis-report (7 leak, 1 false-accept); with it every
// case below gives the spec-stated result, matching the bezelc AOT behaviour.
int run(const std::string& prog) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(prog, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);   // the pass #1521 lives in
    if (diags.hasErrors()) return -999;
    bezel::test::ensureNativeTargetInitialized();
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return -999;
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return -999; }
    auto jit = std::move(*jitOrErr);
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    auto fn = sym->toPtr<int (*)()>();
    return fn();
}

// Cell: a class with a static counter (ctor ++, dtor --) for leak/double-free
// detection. Same idiom as async_owned_task_result_1127_test.cpp's kCell.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// ---------------------------------------------------------------------------
// SUCCESS edge — fast path: Worker's body never touches `c` at all (falls
// off the end, §30.1 "Falling off the end of a `Task`-returning async method
// is equivalent to `return;`"). Pre-fix: hard compile error. Post-fix: `c`
// is auto-dropped by the frame at the success completion edge.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          success_fast_path_fall_off_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    await C.Worker(new Cell()); "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// SUCCESS edge — slow path: Worker suspends via Task.Delay before falling
// off, so the fall-off completion runs through bz_task_complete inside the
// Resume continuation (a pool thread) rather than synchronously inline. Must
// dispose identically to the fast path above.
BEZEL_TEST(async_owned_param_completion_edges_1521,
          success_slow_path_fall_off_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    await Task.Delay(5); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    await C.Worker(new Cell()); "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// SUCCESS edge — explicit `return;` (not just fall-off) with `c` untouched.
BEZEL_TEST(async_owned_param_completion_edges_1521,
          success_explicit_return_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    return; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    await C.Worker(new Cell()); "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// FAULT edge — synchronous throw: Worker throws BEFORE any `await` (the
// §30.1 synchronous prefix — "Calling an async method begins execution
// synchronously on the calling thread until the first `await`"), so the
// fault reaches the `async.fault` boundary on the SAME synchronous call that
// constructed the frame. §30.1: "on every completion path (success, fault,
// and cancellation)". Pre-fix (once the sema hard-error is worked around):
// `endState` drops a throw-only-exit symbol entirely (the dataflow's
// synchronous-body exception-discharge assumption), so `c` was never even
// FLAGGED for frame disposal — a leak, not merely the original hard error.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          fault_synchronous_throw_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await C.Worker(new Cell()); "
        "    catch e: InvalidOperationException => "
        "      _ := 0; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// FAULT edge — throw AFTER a real suspend: Worker awaits (genuinely
// suspending, resumed on a pool thread) and THEN throws, so the fault
// reaches the `async.fault` boundary from inside the Resume continuation,
// not the synchronous entry call. Exercises the exact cross-thread path
// §30.13's "at a suspension point, other continuations ... may dispose
// objects a suspended frame holds" describes for the frame's OWN parameter.
BEZEL_TEST(async_owned_param_completion_edges_1521,
          fault_after_suspend_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    await Task.Delay(5); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      await C.Worker(new Cell()); "
        "    catch e: InvalidOperationException => "
        "      _ := 0; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// CANCELLATION edge — §30.8: the runtime models cancellation as an
// `OperationCanceledException` fault (`CancellationToken.
// ThrowIfCancellationRequested()` throws it synchronously), so this exercises
// the SAME `async.fault` boundary as the fault tests above but via the real
// stdlib cancellation API rather than an arbitrary throw — the literal
// "cancellation" word in §30.1's "success, fault, and cancellation" triad.
// `tok` is the single stdlib-certified borrow-bearing exception to §30.1's
// parameter-capture rule (CancellationToken, §30.1/§30.8); `c` is the
// `owned` parameter under test.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          cancellation_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell, "
        "                                     tok: CancellationToken): Task "
        "    tok.ThrowIfCancellationRequested(); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    try "
        "      await C.Worker(new Cell(), cts.Token); "
        "    catch e: OperationCanceledException => "
        "      _ := 0; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// ABANDONMENT edge — §30.1 "Disposing a not-yet-settled task does not cut
// this short: the underlying operation runs to its terminal state (§30.5),
// and the frame's parameters are disposed then." `t` is dropped (`using
// var` scope exits without an `await`) while Worker is GENUINELY still
// in-flight (suspended on Task.Delay, task->result not yet set) — the task
// object's own disposal must NOT free the frame or its captured `c` early;
// `c` must survive until the frame itself reaches its terminal state on the
// background thread. The `mid` read below (immediately after the drop,
// before the delay elapses) pins that `c` is deliberately still ALIVE at
// that point — the negative half of the exactly-once assertion (not freed
// too early); the second read (after a comfortable margin) pins the disposal
// eventually happens (not leaked forever).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          abandoned_task_disposes_once_at_terminal_state) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    await Task.Delay(30); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.Worker(new Cell()); end; "  // abandoned, in-flight
        "    if Cell.n <> 1 then return -1; end; "  // still alive right after drop
        "    await Task.Delay(150); "  // let the background completion run
        "    return Cell.n; "          // expect 0 (disposed at terminal state)
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// REGRESSION — explicit discharge via `.Dispose()` inside the body must NOT
// be auto-dropped a second time by the frame (single dispose, not a leak
// via skipped auto-drop and not a double-free via BOTH mechanisms firing).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          explicit_dispose_not_double_freed) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): Task "
        "    c.Dispose(); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    await C.Worker(new Cell()); "
        "    return Cell.n; "  // expect 0 (from the explicit Dispose alone)
        "  end Main; "
        "end C;"),
        0);
}

// REGRESSION — moved out via `return c;` through an `OwningTask<T>`
// (Pattern B): §30.5 "chaining ... is an ordinary §28.3 move-out of the
// parameter ... so §30.1's dispose-at-terminal-state rule for not-moved-out
// `owned` parameters does not apply to it". The frame must NOT auto-drop a
// parameter the body already moved out (that would double-free the value the
// caller now owns via the task's result).
BEZEL_TEST(async_owned_param_completion_edges_1521,
          moved_out_result_not_double_freed) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell): OwningTask<Cell> "
        "    return c; "
        "  end Worker; "
        "  public static async method Helper(): Task "
        "    using var result := await C.Worker(new Cell()); "
        "  end Helper; "  // result disposed here — the ONLY dispose
        "  public static async method Main(): Task<int> "
        "    await C.Helper(); "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// REGRESSION (superseded by #2310) — the #973a test the issue tracked as
// the "still_rejected" boundary for the `c.Dispose()` discharge form in a
// plain `async` method. #2310 widens the §29.8/§30.1 admission from the
// marked-adoption-only subset to all six discharge forms (including
// `p.Dispose()` / `p?.Dispose()`): each lowering site nulls the FRAME
// capture slot beside the caller's alloca, so the frame's null-guarded
// terminal-state disposal skips on the discharging branch and disposes
// exactly once on the fall-through. The wider shape is exercised by the
// dedicated `state_machine_param_conditional_discharge_2310` tests in
// tests/ownership/; this test is now an ADMIT-side check (Cell.n == 0
// pins EXACTLY-once disposal).
BEZEL_TEST(async_owned_param_completion_edges_1521,
          conditionally_discharged_param_admitted_after_2310) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Worker(owned c: Cell, flag: bool): Task "
        "    if flag then "
        "      c.Dispose(); "
        "    end; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    await C.Worker(new Cell(), true); "
        "    await C.Worker(new Cell(), false); "
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);  // admitted (#2310): Cell.n == 0 means no leak and no extra disposal
}

// ---------------------------------------------------------------------------
// RELATED FIX — same gate, sync ITERATOR shape: a §29.8 iterator whose body
// ALWAYS exits via throw (never yields) previously leaked its never-
// discharged `owned` parameter for the identical reason the async fault edge
// above did (the throw-only-exit symbol never reaches `endState`, so it was
// never flagged `iteratorAutoDispose` even though `sawYield_` alone already
// permitted the carve-out). The supplemental parameter scan this fix adds is
// gated `sawYield_ || isAsync_`, so it closes this latent sync-iterator gap
// as the same code change — tested here as the "whole enabled surface" this
// touches, not a separate feature.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_param_completion_edges_1521,
          sync_iterator_throw_only_body_disposes_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; " + std::string(kCell) +
        "class C "
        "  public static method Gen(owned c: Cell): IEnumerable<int> "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Gen; "
        "  public static method Main(): int "
        "    try "
        "      for var x in C.Gen(new Cell()) do end; "
        "    catch e: InvalidOperationException => "
        "      _ := 0; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}
