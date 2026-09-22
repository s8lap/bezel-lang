// Ownership — fix #4463: a Pattern-A `Task<T:class>` async producer's
// non-`await` `return` through a WRAPPER AST shape was not caught by either
// `fix #4285` (codegen: MemberAccessExpr / NullCondMemberExpr / IndexExpr at
// top level) or `fix #4311` (ownership: bare IdentifierExpr at top level).
// Neither fix recurses into wrapper expressions or recognizes the wrapper
// shapes themselves.
//
// Spec (revision v0.306):
//   §30.5 "Result lifetime and task disposal", *Pattern A result slots are
//   of three kinds*, kind 1: "A fresh owned value (a `return` of an owned
//   expression): the task owns the result and its destructor disposes it
//   directly." No fourth kind exists, and a bare (non-`await`) return must
//   satisfy kind 1.
//   §28.3 "Local variables, ownership, and borrows", *Producers* table:
//     "`x as T`: Borrowed (source: same as `x`'s source if `x` is borrowed;
//       `x` itself if `x` is owned)"
//     "(if c then v1 else v2) … The meet of the ownership states of v1 and
//       v2 … An unmarked read of a named owned binding remains a borrow arm"
//     "a ?? b … The meet of the ownership states of a and b … A named owned
//       binding written as an arm is an unmarked read and therefore a borrow arm"
//     "Captured owned local inside a closure: Borrowed (source: the local's
//       enclosing scope). Capturing does not transfer ownership; the closure
//       cannot outlive the local's scope."
//     "Plain concrete class or interface type (a static or instance call
//       result): Borrowed … row 4"
//
// THE DEFECT. `checkAsyncPatternABorrowedIdentifierReturn` (Ownership.cpp)
// only inspected a bare IdentifierExpr at the top level — wrapper AST shapes
// that can only unwrap to a borrowed source were invisible to the check:
//   - TypeTestExpr (op="as") — `(x as Cell)!`
//   - ConditionalExpr — `(if c <> null then c else c)`
//   - BinaryExpr (op="??") — `(c ?? c)!`
//   - CallExpr (closure capturing borrow) — `f()` where `f` captures a borrow
//   - CallExpr (instance method returning borrow) — `h.Peek()`
//   - NullCondCallExpr — `h?.Peek()!`
//
// THE FIX (Ownership.cpp, new `exprIsBorrowedSource` recursive helper inside
// `checkAsyncPatternABorrowedIdentifierReturn`): walk through wrapper AST
// shapes recursively to the underlying borrowed source.  The codegen guard
// `detectAsyncPatternAKind` (LlvmBackend_Async.cpp) remains as the fast
// first-pass for the unambiguous top-level MemberAccessExpr /
// NullCondMemberExpr / IndexExpr shapes.
//
// MEASURED (L4 adversarial discovery, probes 3/4/5/7/9/10, 300 iterations
// each under ASan): every wrapper shape above triggers the SAME
// heap-use-after-free (`bz$elemDispose` <- `bz_task_destroy` <-
// `bz$async$M.C$Main$Resume`) — identical to fix #4285's own measured
// defect.
//
// FAIL-WITHOUT / PASS-WITH (measured via commit-fix, revert-src, rebuild,
// re-run, restore-src, rebuild cycle): each of the 6 reject rows below
// compiles CLEAN without this fix and is rejected with a real location and
// the §30.5 diagnostic text WITH it.  The accept-side control rows pass
// identically on both compilers.

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
// deterministic dispose-exactly-once verification.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// A plain (non-owned, non-tag) Cell for the reject rows.
const char* kPlainCell =
    "class Cell public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; end Cell; ";

// The diagnostic fragment shared with fix #4285 / #4311.
constexpr const char* kFixDiagFragment =
    "§30.5 Pattern A) may only `return` an OWNED expression";
}  // namespace

// ---------------------------------------------------------------------------
// Reject rows (fix #4463's 6 wrapper shapes).
// ---------------------------------------------------------------------------

// Probe 3: TypeTestExpr (op="as") wrapping a borrowed parameter.
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_type_test_expr_as_borrow_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method BorrowFromCast(x: Object): Task<Cell> "
        "    return (x as Cell)!; "  // TypeTestExpr (op="as") — borrow per §28.3
        "  end BorrowFromCast; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Probe 4: ConditionalExpr both arms borrowed (same borrowed parameter).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_conditional_expr_both_arms_borrowed_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method BorrowFromConditional(c: Cell): Task<Cell> "
        "    return (if c <> null then c else c); "  // ConditionalExpr — borrow
        "  end BorrowFromConditional; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Probe 5: BinaryExpr (op="??") both arms borrowed (same borrowed parameter).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_binary_expr_null_coalesce_borrowed_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method BorrowFromCoalesce(c: Cell?): Task<Cell> "
        "    return (c ?? c)!; "  // BinaryExpr (op="??") — borrow per §28.3
        "  end BorrowFromCoalesce; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Probe 7: CallExpr over closure capturing a borrow.
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_closure_call_returning_borrowed_capture_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class C "
        "  public static async method BorrowFromCapturedLocal(c: Cell): Task<Cell> "
        "    var f: () -> Cell := lambda() => c; "  // closure captures c (borrow)
        "    return f(); "  // CallExpr — returns borrow from captured local
        "  end BorrowFromCapturedLocal; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Probe 9: CallExpr over instance method returning borrow (receiver is borrowed param).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_instance_method_call_returning_borrow_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class Holder "
        "  public cell: Cell; "
        "  public constructor(c: Cell) this.cell := c; end constructor; "
        "  public method Peek(): Cell => cell; "  // returns a borrow
        "end Holder; "
        "class C "
        "  public static async method BorrowFromInstanceCall(h: Holder): Task<Cell> "
        "    return h.Peek(); "  // CallExpr over MemberAccessExpr — borrow
        "  end BorrowFromInstanceCall; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// Probe 10: NullCondCallExpr over instance method returning borrow.
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          reject_null_cond_call_returning_borrow_30_5) {
    auto r = runAot(
        "module M; " + std::string(kPlainCell) +
        "class Holder "
        "  public cell: Cell; "
        "  public constructor(c: Cell) this.cell := c; end constructor; "
        "  public method Peek(): Cell => cell; "  // returns a borrow
        "end Holder; "
        "class C "
        "  public static async method BorrowFromNullCondCall(h: Holder?): Task<Cell> "
        "    return h?.Peek()!; "  // NullCondCallExpr + PostfixBangExpr — borrow
        "  end BorrowFromNullCondCall; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFixDiagFragment));
}

// ---------------------------------------------------------------------------
// Accept-side controls (must keep compiling with the fix).
// ---------------------------------------------------------------------------

// A ConditionalExpr returning a MOVED owned local: the unconditional arm's
// move means the result is owned, admitted as kind 1. Confirms no regression
// from the ConditionalExpr recursive walk. Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          accept_conditional_expr_owned_arms_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method CondOwned(): Task<Cell> "
        "    var x := new Cell(); "
        "    return (if true then move x else move x); "
        "  end CondOwned; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.CondOwned(); var x := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}

// A BinaryExpr (op="??") returning a MOVED owned local: both arms are moves,
// the result is owned, admitted as kind 1. Confirms no regression from the
// BinaryExpr recursive walk. Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          accept_null_coalesce_owned_arms_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method CoalesceOwned(): Task<Cell> "
        "    var x := new Cell(); "
        "    return (move x ?? move x); "
        "  end CoalesceOwned; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.CoalesceOwned(); var x := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}

// A CallExpr returning an owned Cell via a factory method: the factory
// returns `owned Cell` per §28.3 *Factory methods and sourceless returns*.
// Confirms no regression from the CallExpr recursive walk.
// Loop-amplified (50x).
BEZEL_TEST(async_pattern_a_wrapper_shapes_4463,
          accept_call_expr_owned_result_unaffected_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class Factory "
        "  public static method Make(): owned Cell => new Cell(); "
        "end Factory; "
        "class C "
        "  public static async method CallOwnedFactory(): Task<Cell> "
        "    return Factory.Make(); "
        "  end CallOwnedFactory; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 50 do "
        "      do using var t := C.CallOwnedFactory(); var x := await t; end; "
        "      i := i + 1; "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}
