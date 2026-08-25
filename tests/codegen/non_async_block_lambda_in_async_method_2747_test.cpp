// Codegen — §30.1 / §30.5 / §27.5 (issue #2747): a NON-async BLOCK-bodied
// lambda (`lambda(x: int) => do return x + 1; end`) used anywhere inside
// an `async method` body must lower to valid IR and produce the expected
// runtime value.
//
// Regression for #2747. The bug has TWO related surfaces — same root
// cause, different symptoms — both a rejected-valid (compile abort) and
// a miscompile (compile succeeds but the binary SEGVs / returns wrong).
//
// Root cause: `lowerLambda`'s SYNC branch (the non-`isAsync` arm) saves
// and clears the enclosing machine's `smKind_` / `smStateStruct_` /
// `smSelf_` / …  before emitting the body, and restores them after.
// The previous fix (#3237) added that save/clear/restore for the
// `sm*_/async*_` state (the `emitAsyncComplete`-via-`smSelf_` cross-
// function GEP class, symptom (a)). But it did NOT save and reset the
// two Pattern A detection flags `asyncChainedReturn_` and
// `asyncBorrowedReturn_` that `lowerReturnStmt` reads and RESETS at
// `LlvmBackend_Stmts.cpp:1928-1937` UNCONDITIONALLY on every return —
// even when the inner return is a plain `return <int expr>` with no
// `await`. When the enclosing async method's `return await <call>`
// detection sets one of those flags BEFORE the lambda body is lowered
// (the lambda sits in the operand of the awaited call, e.g.
// `return await C.Inner(C.Apply(lambda(...) => do return x + 1; end,
// 6))`), the inner `lowerReturnStmt` runs, sees its own non-await
// `return x + 1` (so does NOT set the flag again), then unconditionally
// reads the OUTER's flag at line 1928 and RESETS it to false at line
// 1929 — clobbering the outer method's chained/borrowed return state.
// After `lowerLambda` returns, the outer `lowerReturnStmt` consults the
// now-false flag and treats its chained slot as a plain borrow, so the
// source task is free-on-completion and the caller's scope-end dispose
// sees a freed cell (SEGV / wrong return).
//
// This file pins BOTH symptoms:
//   * The simple shape from the issue body (lambda bound to a local
//     inside an async method). Symptom (a) was already fixed by #3237;
//     this test pins that simple shape as a regression canary.
//   * The harder shape (lambda inside the operand of a `return await
//     <call>`). This is symptom (b) — the SEGV — and is what the
//     `asyncChainedReturn_` / `asyncBorrowedReturn_` save/clear/restore
//     in this fix closes. Pre-fix: compile succeeds but the binary
//     SEGVs (the source task is freed while the outer caller still
//     references it). Post-fix: returns the expected cell value.
//   * The control cases (expression-bodied lambda in the same position;
//     block-bodied lambda inside a synchronous method called from the
//     async body) all already worked and must not regress.
//
// All run via `runAot` (the AOT oracle). The exit code is the int
// returned by `Main(): Task<int>`. ASan is opt-in via the lane protocol
// and is not wired here — the bug is a memory-safety failure on the
// async-completion path that surfaces without sanitizer instrumentation.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "codegen/LlvmBackend.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

static const fs::path& wtDir2747() {
    return wtScratchDir("wt-2747");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// exit code is `Cell.n * 100 + Cell.made`. After `new Cell(7)` (via
// lambda body `x + 1` = 7), then explicit dispose via `using var`, we
// expect 0*100 + 1 = 1: the cell was constructed exactly once and
// disposed exactly once.
void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #2747 (symptom b — the SEGV). A non-async block-bodied lambda passed as
// the argument of a call that is itself the argument of `return await
// <call>` inside an async method. Pre-fix: compile succeeds but the
// binary SEGVs (outer's `asyncChainedReturn_` flag is clobbered by the
// lambda body's own `lowerReturnStmt` at
// `LlvmBackend_Stmts.cpp:1928-1937`, so the source task is freed while
// still chained). Post-fix: returns 1.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(non_async_block_lambda_in_async_method_2747,
           block_lambda_inside_return_await_operand) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; Cell.made := Cell.made + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Apply(g: (int) -> int, n: int): int "
        "    return g(n); "
        "  end Apply; "
        "  public static async method Inner(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end Inner; "
        "  public static async method Outer(): Task<Cell> "
        "    return await C.Inner(C.Apply(lambda(x: int) => do return x + 1; end, 6)); "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.Outer(); "
        "      var r := await t; "
        "    end; "
        "    return Cell.n * 100 + Cell.made; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// #2747 (symptom a — the verifier failure). The bug's headline shape:
// the same block-bodied lambda bound to a local var inside an async
// method body, then used at the await site. Symptom (a) was already
// fixed by #3237 (the sm*_/async*_ save/clear); this pins it so the
// next regression in that same guard doesn't quietly reintroduce the
// "Function return type does not match operand type of return inst!"
// verifier abort. Returns 1 like the issue's "Controls" note.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(non_async_block_lambda_in_async_method_2747,
           block_lambda_bound_to_local_then_awaited) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; Cell.made := Cell.made + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Apply(g: (int) -> int, n: int): int "
        "    return g(n); "
        "  end Apply; "
        "  public static async method Inner(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end Inner; "
        "  public static async method Outer(): Task<Cell> "
        "    var k := C.Apply(lambda(x: int) => do return x + 1; end, 6); "
        "    return await C.Inner(k); "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.Outer(); "
        "      var r := await t; "
        "    end; "
        "    return Cell.n * 100 + Cell.made; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// #2747 control — expression-bodied lambda at the headline position.
// Must continue to compile and return the same value (1). Pins the
// expr-body path (`lowerExpr(lam.exprBody)`) does not lose the flag
// either; the fix is scoped to the SYNC branch's save/clear/restore,
// which the expr-body path also walks.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(non_async_block_lambda_in_async_method_2747,
           expr_lambda_inside_return_await_operand_control) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public static made: int := 0; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; Cell.made := Cell.made + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Apply(g: (int) -> int, n: int): int "
        "    return g(n); "
        "  end Apply; "
        "  public static async method Inner(k: int): Task<Cell> "
        "    return new Cell(k); "
        "  end Inner; "
        "  public static async method Outer(): Task<Cell> "
        "    return await C.Inner(C.Apply(lambda(x: int) => x + 1, 6)); "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.Outer(); "
        "      var r := await t; "
        "    end; "
        "    return Cell.n * 100 + Cell.made; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 1);
}
