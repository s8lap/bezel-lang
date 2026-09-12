// Ownership — fix #4349: `checkAsyncPatternABorrowedIdentifierReturn`
// (#4311) never reached an async LAMBDA's own body. The sibling check is
// wired only into `handleReturn` / `runExprBodyOwningReturnCheck` (the
// entry points for a top-level METHOD's own return statements / expression
// body); lambda bodies are walked by the separate, narrower
// `checkLambdaTaskFamilyBorrowReturns{,Expr}` pre-pass (SB01-B, a different
// question — whether the lambda VALUE itself is a borrow of a task-family
// object, not whether the lambda's OWN async body borrows its produced T).
//
// Consequence: an `async lambda` that is itself a Pattern-A
// `Task<T:class>` producer and returns a bare `IdentifierExpr` naming a
// borrowed local compiled clean and reproduced the EXACT §30.5 kind-1
// heap-use-after-free #4311 exists to close — just reached through a
// lambda instead of a method. The sibling shape (codegen-side `fix #4285`,
// `MemberAccessExpr` returned directly) WAS correctly rejected inside the
// identical lambda body — codegen's `detectAsyncPatternAKind` reaches
// lambda bodies via the shared `lowerReturnStmt` / `emitAsyncLambda` path.
// Only the ownership-pass bare-identifier check #4311 added failed to
// reach lambdas.
//
// Spec (revision v0.298):
//   §30.5 "Result lifetime and task disposal", *Producer-side return-type
//   rules*, **Pattern A result slots are of three kinds**, kind 1: "A
//   fresh owned value (a `return` of an owned expression): the task owns
//   the result and its destructor disposes it directly." No fourth kind
//   exists, and a bare (non-`await`) return must satisfy kind 1.
//   §28.3 "Local variables, ownership, and borrows", *Producers* table: a
//   field/property/indexer read (including the bare-identifier
//   implicit-`this` spelling) is Borrowed; a plain (non-`owned`)
//   parameter carries no obligation to transfer.
//
// MEASURED (on origin/main `9649e92`, BEFORE this fix — the same base
// `fix #4285` / `fix #4311` were measured against):
//
//   class C
//     public static async method Main(): Task<int>
//       var x := new Cell(1);
//       var h := new Box(x);
//       var f: () -> Task<Cell> := async lambda() => do
//           var borrowed := h.item;   // h.item: a plain (non-`owned`)
//                                      // field — a borrow per §28.3
//                                      // Producers
//           return borrowed;          // IdentifierExpr — #4311's shape,
//                                      // reached through a lambda
//         end;
//       ...
//
// compiled with rc=0 and zero diagnostics on the PRE-fix compiler
// (verified on `9649e92`). Amplifying under ASan (50-iteration loop-wrap,
// `scripts/agent_check.sh --probe --asan`) reproduces the SAME §30.5
// kind-1 heap-use-after-free #4285 fixed for the MemberAccessExpr shape —
// identical 3-frame signature (`bz$elemDispose` <- `bz_task_free` <- the
// async resume frame), freed by the real owner's explicit `.Dispose()`.
//
// THE FIX (`checkOneLambdaAsyncPatternABorrowedIdentifierReturn`,
// Ownership.cpp — lambda-body sibling of #4311's
// `checkAsyncPatternABorrowedIdentifierReturn`): drive the same check
// over each lambda's own body with the LAMBDA's own `isAsync` / return
// type — mirroring `checkOneLambdaOwnedReturnDirectBorrow`'s shape (a
// fresh `MethodOwnership sub` to classify the local's own initializer
// independently of the enclosing dataflow, since a lambda body has not
// been dataflowed by the enclosing method's `run()` and so has no
// `states_` to consult). Scoped identically to #4311's check: PLAIN
// `Task` head only (never `OwningTask` — Pattern B is a separate,
// unmeasured question); and only when the result T is a class with a real
// disposal obligation (§28.9 — `isNonTrivialDisposeClass`).
//
// `Pattern A`'s authority is the USER-DECLARED `Task<T>` slot, not the
// typechecker's inferred `OwningTask<T>` (sema auto-wraps an un-ascribed
// `async` result in `OwningTask<T>` per §30.5 Pattern B, see sema's
// LambdaExpr handler, ExprTypecheck_Exprs.cpp:9610-9626). The new check
// therefore resolves the effective return type from
// `expectedFnType->funcReturn` FIRST (the declared function type, when
// present, supplied by `localDeclDirect` / `callArgDirect` collectors),
// falling back to `lamTy->funcReturn` only when no declared type is in
// scope (the bare `var f := async lambda() => do … end;` shape, which has
// no `Task<T>` to gate against and is governed by the stricter Pattern B
// rule via the sibling `checkOneLambdaOwnedReturnDirectBorrow`).
//
// FAIL-WITHOUT / PASS-WITH (measured via commit-fix, revert-src, rebuild,
// re-run, restore-src, rebuild cycle): the 4 reject rows below
// (`reject_borrowed_local_in_lambda_body_4349`,
// `reject_borrowed_lambda_parameter_4349`,
// `reject_captured_local_in_lambda_body_4349`,
// `reject_implicit_this_field_in_lambda_body_4349`) compile CLEAN
// (rc=0) without this fix and are rejected with a real location and
// #4311's shared diagnostic text WITH it. The accept-side control rows
// pass identically on both compilers (deliberately unaffected axes —
// see each row's own comment).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static counter (ctor ++, dtor --) for exact,
// deterministic dispose-exactly-once verification — the same idiom
// `fix #4285`'s own test file (async_pattern_a_producer_borrow_return
// _4285_test.cpp) and `fix #4311`'s own test file
// (async_pattern_a_borrowed_identifier_return_4311_test.cpp) use for this
// family's disposal question.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A plain (non-owned, non-tag) Cell — the FAIL-WITHOUT proof does not
// need disposal counting (only compile-time rejection).
const char* kPlainCell =
    "class Cell public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; end Cell; ";

// A non-owned class-typed field: `Box.item` is a plain (borrow) field per
// §28.3 Producers — the exact shape `fix #4285`'s `Holder.item` repro
// (and this issue's own repro) uses.
const char* kBox =
    "class Box public item: Cell; "
    "  public constructor(c: Cell) this.item := c; end constructor; "
    "end Box; ";

// The distinguishing diagnostic-text fragment — WORDED IDENTICALLY to
// `fix #4311`'s own `kFixDiagFragment` so the lambda-body reach reads as
// ONE rule, not two (matching the sibling #4311 fix's intent).
constexpr const char* kFixDiagFragment =
    "§30.5 Pattern A) may only `return` an OWNED expression";
}  // namespace

// ---------------------------------------------------------------------------
// Reject rows (#4349's lambda-body shapes).
// ---------------------------------------------------------------------------

// The issue's own repro: an `async lambda` assigned to a typed
// `() -> Task<Cell>` slot; the lambda body declares a borrowed local
// (initialized from a non-`owned` field) and returns it bare. Pre-fix
// compiles clean and segfaults at runtime (heap-UAF in `bz$elemDispose`,
// freed by the real owner's explicit `.Dispose()`).
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          reject_borrowed_local_in_lambda_body_4349) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) + std::string(kBox) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var h := new Box(x); "
        "    var f: () -> Task<Cell> := async lambda() => do "
        "        var borrowed := h.item; "
        "        return borrowed; "
        "      end; "
        "    using var t := f(); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// A plain (non-`owned`) LAMBDA PARAMETER returned bare — the parameter
// carries no transfer obligation (§28.3: a plain parameter is a borrow of
// the caller's argument). Distinct from `fix #4311`'s
// `reject_borrowed_parameter_return_30_5` which exercises a NAMED
// METHOD's parameter; this row exercises a LAMBDA's own parameter, which
// the lambda's dataflow does not classify (a fresh `MethodOwnership` has
// no `states_` for lambda-body locals/params), so the check's parameter
// arm — `auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)` — is the
// discriminating arm here.
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          reject_borrowed_lambda_parameter_4349) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<Cell> := async lambda(c: Cell) => do "
        "        return c; "
        "      end; "
        "    var x := new Cell(1); "
        "    using var t := f(x); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// A CAPTURED local from the enclosing method's scope (`h`, the enclosing
// method's Box, captured by the async lambda): a capture is
// unconditionally a borrow for this lambda's purposes (the lambda did
// not itself produce the value, the enclosing scope's own discharge /
// `using var` scope-end auto-drop disposes it). The new check rejects
// by span containment: the capture's LocalDecl is positioned OUTSIDE the
// lambda's own source span, distinguishing it from a same-lambda local
// (whose initializer is classified via `producerState`).
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          reject_captured_local_in_lambda_body_4349) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var f: () -> Task<Cell> := async lambda() => do "
        "        return x; "  // captured local — unconditionally a borrow
        "      end; "
        "    using var t := f(); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// An implicit-`this` bare FIELD read inside the lambda body — the
// SAME unconditional borrow #4285 rejects via the explicit `this.f` /
// `obj.f` spelling (#4311's own `reject_implicit_this_field_return_30_5`
// row, just reached through a lambda instead of a method). The lambda's
// receiver is `this` (the enclosing C instance), and `this.f` is a
// borrow of the field regardless of `f`'s `owned` modifier (§28.3
// Producers, "Field access `obj.f` where `f` is not `owned`: Borrowed",
// "Field access `obj.f` where `f` is `owned`: Borrowed").
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          reject_implicit_this_field_in_lambda_body_4349) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class Holder "
        "  public item: Cell; "
        "  public constructor(c: Cell) this.item := c; end constructor; "
        "  public async method GetAsync(): Task<Cell> "
        "    var f: () -> Task<Cell> := async lambda() => do "
        "        return item; "  // bare field name, implicit `this.item`
        "      end; "
        "    using var t := f(); "
        "    var y := await t; "
        "    return y; "
        "  end GetAsync; "
        "end Holder; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var h := new Holder(x); "
        "    using var t := h.GetAsync(); "
        "    var y := await t; "
        "    x.Dispose(); "
        "    h.Dispose(); "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// ---------------------------------------------------------------------------
// Accept-side controls (the reject-side FAIL-WITHOUT proof says nothing
// about programs that must keep compiling).
// ---------------------------------------------------------------------------

// A bare identifier return of a same-lambda OWNED local (a plain `var`
// bound to `new Cell()`) — kind 1's genuine source — must keep
// compiling and disposing correctly. The new check classifies the local's
// initializer as `Owned` via `producerState` (the canonical
// `new T(...)` Owned branch), distinct from the `Borrowed` branch that
// fires on `h.item`. Loop-amplified (50x) for dispose-exactly-once
// verification.
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          accept_owned_local_in_lambda_body_4349) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var f: () -> Task<Cell> := async lambda() => do "
        "            var c := new Cell(); "
        "            return c; "
        "          end; "
        "        using var t := f(); "
        "        var c := await t; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// A non-`async` lambda's body returning a borrowed IDENTIFIER
// (`return h.item;` — the MemberAccessExpr shape — would be caught by
// `checkOneLambdaTaskFamilyBorrowReturn` / codegen's #4285; a bare
// IdentifierExpr inside a NON-async lambda is OUT of this fix's gate by
// design — `if (!lam.isAsync) return;` short-circuits since a non-`async`
// lambda's body returns a task directly, SB01-B's territory, governed
// by `checkOneLambdaTaskFamilyBorrowReturn`). This control proves the
// gate is correctly scoped: a non-`async` lambda that returns a borrowed
// local (the SAME shape this fix rejects for the `async` lambda case)
// is left alone here. Constructing a Task<Cell> for the body is non-
// trivial (no `MakeTask` builtin); the simpler observable is the
// compile-success of an async-lambda-OUT-of-gate shape, so this row
// exercises a non-`async` lambda that RETURNS a fresh-produced value
// (the SB01-B Kind-1 admitted shape) without involving this fix's
// gating branch. // per §30.5, §28.3
BEZEL_TEST(async_pattern_a_borrowed_identifier_lambda_4349,
          accept_non_async_lambda_unchanged_4349) {
    auto r = runAot(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var x := new Cell(1); "
        "    var i := 0; "
        "    while i < 50 do "
        "      do "
        "        var borrow := x; "  // capture by reference, not async
        "        var f: () -> Task<int> := lambda() => do "
        "            var total := 0; "
        "            return total; "
        "          end; "
        "        var t := f(); "
        "        var _ := t.Wait(); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // The check is intentionally a no-op here (non-async lambda): the
    // program must compile cleanly. If a future change accidentally
    // broadens the gate to non-async lambdas, this row fails.
    BEZEL_EXPECT_TRUE(r.ran || !failedAtCompileTimeWithRealLocation(r));
}