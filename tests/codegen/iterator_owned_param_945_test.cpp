// Codegen + ownership — #945: an iterator method may take `owned` parameters;
// the state machine disposes them at iterator-Dispose (§29.8).
//
// Root cause (pre-fix): the ownership pass treated an iterator body as a plain
// method, so an `owned` parameter that reached the body's normal exit without
// an explicit discharge was rejected with the §28.3 "would be dropped without
// disposal" error. But per §29.8 the iterator's state machine takes ownership at
// construction and disposes the parameter when the iterator is disposed (after
// the body's `using var` / `finally` cleanups, in reverse declaration order).
//
// The fix has two halves (a sema accept WITHOUT the codegen disposal would trade
// a false-reject for a silent leak — the accept-trap the issue warns about):
//   - sema (Ownership.cpp): a SYNC iterator's `owned` parameter that the body
//     NEVER moves out is flagged for state-machine disposal and the reject is
//     suppressed. A parameter discharged on SOME path stays rejected (the state
//     machine cannot statically know it was moved out → auto-disposing would
//     risk a double-free; conservative).
//   - codegen (LlvmBackend_Iterators.cpp): emitIteratorMethod's Dispose disposes
//     each flagged owned parameter (reverse order, null-guarded) before freeing
//     the frame, on every disposal path (exhaustion, break, never-iterated).
//
// Disposal is checked with a static instance counter Cell.n (ctor +1, dtor -1):
// a final 0 means each instance was disposed EXACTLY once (no leak, no
// double-free, no negative count).

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
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
// The #945 fix lives ENTIRELY in the ownership pass (the §28.3 reject + the
// §29.8 auto-dispose flag) and the iterator-Dispose codegen that reads it. The
// shared `runJit` helper runs sema but NOT `ownership::checkOwnership`, so it
// can exercise neither half (the flag stays unset → the param leaks; the reject
// never fires → an invalid program "runs"). This full-pipeline runner adds the
// ownership pass before codegen, mirroring the real driver — without it these
// tests would mis-report. Mirrors codegen_support.hpp's runJitWithOptions plus
// the ownership pass.
int run(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);   // the pass #945 lives in
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
// True iff the full pipeline (including the ownership pass) rejected `src`.
bool rejectedAtCompile(const std::string& src) {
    auto r = bezel::test::parseAndCheckOwnership(src);
    return r.diagnostics.hasErrors();
}
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// §29.8: the issue's repro — an owned param never touched by the body is
// disposed once by the state machine (Cell.n returns to 0).
BEZEL_TEST(iterator_owned_param_945, repro_owned_param_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8: the consumer breaks before exhaustion — the owned param is still
// disposed exactly once at iterator-Dispose (the for-loop's __source discharge).
BEZEL_TEST(iterator_owned_param_945, disposed_on_early_break) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in UseCell(new Cell()) do break; end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8: the body moves the owned param out (`using var local := c`) — the
// state machine must NOT also dispose it (the using-var did). No double-free.
BEZEL_TEST(iterator_owned_param_945, moved_out_param_not_double_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    using var local := move c; "
        "    yield return 1; yield return 2; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8 / §28.4: multiple owned params are each disposed once (reverse order).
BEZEL_TEST(iterator_owned_param_945, two_owned_params_each_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method Use2(owned a: Cell, owned b: Cell): IEnumerable<int> "
        "    yield return 1; "
        "  end Use2; "
        "  public static method Main(): int "
        "    for var x in Use2(new Cell(), new Cell()) do end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8: the body BORROWS the owned param (reads c.V) without discharging it —
// the param is still auto-disposed (no false-reject), and the borrow reads work.
BEZEL_TEST(iterator_owned_param_945, borrow_use_auto_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in UseCell(new Cell()) do sum := sum + x; end; "
        "    return sum + Cell.n; "  // 7+7 + 0 == 14
        "  end Main; end P;"), 14);
}

// §29.8 (conservative): a CONDITIONALLY-discharged owned param used to be
// rejected outright — the state machine could not statically tell whether the
// body moved it out, so auto-disposing risked a double-free.
//
// §28.10 v0.180 / #2071: the unmarked `using var x := c;` form is a borrow-pin
// over a named owned binding (§28.10), so the conservative conditional-adoption
// shape no longer applies — the source is never moved by the using-var and the
// state machine auto-disposes it. The conservative rejection lived on in the
// MARKED `using var x := owned c;` form.
//
// #973a: the MARKED conditional adoption is now ACCEPTED. §29.8's rule is
// "disposes any `owned` parameter that has not been moved out of by the body" —
// a runtime property — so the frame's capture slot became the runtime moved-from
// flag (nulled at the adoption, read by the already-null-guarded frame
// disposal). See `iterator_owned_param_conditional_973a_test.cpp` for the
// exactly-once behaviour on both branches, on every teardown path, and for the
// discharge forms that DO still keep the reject (`c.Dispose()`, `_ := c`,
// `M(owned c)` — each nulls only the caller's alloca, not the frame slot).
BEZEL_TEST(iterator_owned_param_945, conditional_discharge_now_accepted_973a) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        "module M; "
        "class Cell public constructor() end constructor; "
        "  destructor end destructor; end Cell; "
        "class P "
        "  public static method UseCell(owned c: Cell, cond: bool): IEnumerable<int> "
        "    if cond then using var x := owned c; end; "
        "    yield return 1; "
        "  end UseCell; "
        "  public static method Main(): int return 0; end Main; end P;"), false);
}

// §29.8 (#973b): an ASYNC iterator's never-discharged `owned` param is no longer
// rejected — it is auto-disposed by the async-iterator Dispose (emitFreeAndReturn,
// on every Dispose path). Accept-check (ownership half): the program compiles.
BEZEL_TEST(iterator_owned_param_945, async_iterator_owned_param_accepted_973b) {
    BEZEL_EXPECT_FALSE(rejectedAtCompile(
        std::string("module M; ") + kCell +
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    await for var x in UseCell(new Cell()) do end; "
        "    return 0; "
        "  end Main; end P;"));
}

// §29.8 (#973b): runtime — the async-iterator owned param is disposed EXACTLY once
// (Cell.n returns to 0). No-await iterator body so the JIT test is timer-free.
BEZEL_TEST(iterator_owned_param_945, async_iterator_owned_param_disposed_once_973b) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    await for var x in UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0 — c disposed exactly once at iterator Dispose
        "  end Main; end P;"), 0);
}

// §29.8 (#973b): the consumer BREAKS before exhaustion — the async iterator's
// owned param is still disposed exactly once on the abandonment Dispose path.
BEZEL_TEST(iterator_owned_param_945, async_iterator_owned_param_disposed_on_break_973b) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    await for var x in UseCell(new Cell()) do break; end; "
        "    return Cell.n; "  // 0 — disposed once even on early abandonment
        "  end Main; end P;"), 0);
}

// §29.6 / §29.8 (#945, adversarial-review Finding 1): the owned param is
// consumed INSIDE a yield operand — `yield return Consume(c)` where Consume
// takes and disposes it. The discharge must be recorded (visitStmt's YieldStmt
// handler) so the state machine does NOT also dispose it. No double-free.
BEZEL_TEST(iterator_owned_param_945, owned_param_consumed_in_yield_operand) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  private static method Consume(owned x: Cell): int "
        "    using var u := move x; return 1; "
        "  end Consume; "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return Consume(move c); "  // §22.3 v0.169: named owned binding
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var v in UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0 — Consume disposed c once; no double-free
        "  end Main; end P;"), 0);
}

// §29.6: yielding the BARE owned param (`yield return c`) is a read, not a
// discharge (the enumerator does not separately reclaim it), so the state
// machine disposes it exactly once. No double-free, no leak.
BEZEL_TEST(iterator_owned_param_945, bare_owned_param_yielded_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<Cell> "
        "    yield return c; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §28.3 v0.152 (#1008 Part B, owned-parameter auto-drop): a NON-iterator method
// with an undischarged owned param is no longer rejected — it auto-drops the
// param at method end (disposed EXACTLY once; Cell.n returns to 0), the same
// implicit disposal the iterator state machine performs at iterator-Dispose.
// (Pre-v0.152 this was a hard §28.3 reject; the #945 accept was iterator-scoped.)
BEZEL_TEST(iterator_owned_param_945, non_iterator_owned_param_auto_dropped) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): int "
        "    return 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    var k := UseCell(new Cell()); "
        "    return Cell.n; "  // 0 — auto-dropped exactly once
        "  end Main; end P;"), 0);
}
