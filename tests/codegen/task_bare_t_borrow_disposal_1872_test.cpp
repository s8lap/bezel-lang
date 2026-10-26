// Codegen/runtime — issue #1872 (§28.3 v0.190 SB-01 "Calls mint, storage
// lends"): an EXOTIC task-typed local whose CALL initializer producerState
// classifies BORROWED must NOT register its own scope-end task disposal.
//
// #1870 fixed the borrowed/aliased task-local double-free by gating the plain-
// `var` scope-end `d.task` (LlvmBackend_Stmts.cpp) on initializer SHAPE: a
// CallExpr initializer keeps `d.task` (a mint — "calls mint"), an alias /
// storage read is skipped (a borrow — "storage lends"). But producerState
// (src/ownership/Ownership.cpp) classifies a few CALL shapes as **Borrowed**
// that the shape gate did NOT replicate, so codegen OVER-KEPT `d.task` for them
// — a second `bz_task_free` on a frame owned elsewhere → a double-free that
// corrupts async continuation state (symptom: a HANG; ASan traps no report
// before it), or, with a class result, disposes the result twice:
//
//   1. A bare-type-parameter-return call `First<T>(a): T` instantiated at
//      `T = Task<int>` / `Task<Cell>` — ownership governed by §28.3
//      *Bare-type-parameter returns* (#1801): the result BORROWS the argument-
//      source union, so the argument (`a`), not the local, owns the frame.
//   2. An owning-wrapper method returning a bare receiver owned-field (the
//      #1493 shape) — producerState downgrades the call to Borrowed.
//
// Both pre-existed on `main` (out of the #1786-enabled surface). Case 1 is the
// reachable one and is pinned here directly. Case 2 for the task heads
// (`OwningTask`/`Task`) is REJECTED at the DEFINITION site — the #1493 receiver-
// owned-field carve-out is excluded for the deferred-activation owning heads
// (#1660, checkBorrowReturnedThroughOwningType), so such a method cannot
// compile and the double-free is unreachable via the `d.task` gate today; the
// last test pins that rejection. The fix still covers it defensively (should
// #1660 ever be relaxed) via the same recognizer producerState uses.
//
// Fix (durable): the ownership pass threads its Owned-vs-Borrow verdict for
// these two CALL shapes onto the LocalDecl (`initializerCallIsBorrow`, set with
// the SAME `callDeclaredReturnIsBareTypeParam` / `methodReturnsOnlyWrapper-
// ReceiverBorrow` recognizers producerState uses); codegen's `d.task` gate
// consults it instead of re-deriving ownership from the AST shape — so codegen
// and the ownership pass agree on who holds the disposal obligation. A genuine
// Task-family mint call (a user `async` method, `Task.FromResult`) stays Owned/
// unflagged and keeps its `d.task`. // per §28.3 v0.190 SB-01 (#1872), #1801,
// #1493, #1660

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAot with the compiled-program run deadline in force: a HANG surfaces as
// ran=false (the child is SIGKILLed on the deadline), which this maps to a
// sentinel distinct from any real exit code.
int run(const std::string& prog) {
    JitResult r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

bool anyDiagContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Cell: static counter (ctor ++, dtor --) + a "DISP" stdout print per disposal.
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; Console.WriteLine(\"DISP\"); "
    "  end destructor; "
    "end Cell; ";

}  // namespace

// ---------------------------------------------------------------------------
// Case 1 (class task) — the core bug. `var a := Producer(); var t := First(a);`
// where `First<T>(a: T): T => a` is a bare-type-parameter-return call at
// `T = Task<Cell>`. `t` BORROWS `a`'s frame (#1801); only `a` (the mint CALL,
// auto-dropped at scope end since its live range never crosses an await, §28.12)
// owns and disposes it — disposing its Cell EXACTLY once. WITHOUT the fix `t`
// ALSO registered `d.task`, double-freeing the frame: the Cell is disposed twice
// (or the async state corrupts and the program HANGS) — `run()` returns the -999
// hang sentinel / a nonzero exit. WITH the fix: exit 0, exactly one "DISP".
// ---------------------------------------------------------------------------
BEZEL_TEST(task_bare_t_borrow_disposal_1872,
           class_task_bare_t_borrow_disposed_exactly_once_28_3_v0190) {
    const std::string prog =
        "module M; " + std::string(kCell) +
        "class C "
        "  public static method First<T>(a: T): T => a; "
        "  public static async method Producer(): Task<Cell> "
        "    return new Cell(); "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    do var a := Producer(); var t := First(a); end; "
        "    return Cell.n; "   // 0 == disposed EXACTLY once (not twice)
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 0);   // no hang (-999), no imbalance

    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);              // ASan-clean: no double-free / UAF
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DISP"), 1);  // NOT 2
}

// ---------------------------------------------------------------------------
// Case 1 (value task) — the same double-free of the task FRAME with no class
// result to count. `var a := AsyncProd(); var t := First(a);` — `t` borrows
// `a`'s `Task<int>` frame; only `a` disposes it. WITHOUT the fix both register
// `d.task` -> double-free -> the async scheduler corrupts and the program HANGS
// (-999). WITH the fix: runs, exit 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(task_bare_t_borrow_disposal_1872,
           value_task_bare_t_borrow_not_double_freed_28_3_v0190) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method First<T>(a: T): T => a; "
        "  public static async method AsyncProd(): Task<int> "
        "    return 7; "
        "  end AsyncProd; "
        "  public static async method Main(): Task<int> "
        "    do var a := AsyncProd(); var t := First(a); end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 0);   // no hang (-999)

    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);              // ASan-clean: no double-free of frame
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// No-new-leak control: a genuine minted owner (a Task-family CALL — a user
// `async` method) is classified Owned, is NOT flagged a borrow, and KEEPS its
// scope-end `d.task`, disposing its Cell EXACTLY once. This guards the fix
// against over-suppressing `d.task` for real owners (which would leak). The
// awaited owner's frame disposes its Cell; net Cell.n == 0, one "DISP".
// ---------------------------------------------------------------------------
BEZEL_TEST(task_bare_t_borrow_disposal_1872,
           class_task_minted_owner_disposed_exactly_once_28_3_v0190) {
    const std::string prog =
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Producer(): Task<Cell> "
        "    return new Cell(); "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    do var t := Producer(); var c := await t; end; "
        "    return Cell.n; "   // 0 == owner disposed its Cell exactly once
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 0);

    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);              // compiled + ASan-clean (no leak)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DISP"), 1);  // exactly once
}

// ---------------------------------------------------------------------------
// Case 2 unreachability pin: an `OwningTask`-returning method that returns a
// bare receiver owned-field (`=> _t;`) — the #1493 wrapper-receiver-borrow shape
// — is REJECTED at the DEFINITION site for the deferred-activation owning heads
// (#1660): the caller cannot in general resolve the concrete all-borrow method,
// so the borrow could be laundered into a second owner under indirect dispatch.
// Because the method cannot compile, no `OwningTask`-typed local can be
// initialized by such a call, and the case-2 `d.task` double-free is unreachable
// on current `main`. This pins that rejection; the #1872 fix still covers the
// shape defensively (via methodReturnsOnlyWrapperReceiverBorrow) should the
// definition-site rule ever be relaxed. // per §28.3/§5.11 (#1493, #1660)
// ---------------------------------------------------------------------------
BEZEL_TEST(task_bare_t_borrow_disposal_1872,
           owningtask_receiver_field_borrow_return_rejected_at_definition_1660) {
    const std::string prog =
        "module M; " + std::string(kCell) +
        "class Holder "
        "  public owned _t: OwningTask<Cell>; "
        "  public constructor(owned t: OwningTask<Cell>) _t := move t; "
        "  end constructor; "
        "  destructor end destructor; "
        "  public method Peek(): OwningTask<Cell> => _t; "
        "end Holder; "
        "class C "
        "  public static async method Main(): Task<int> return 0; end Main; "
        "end C;";
    JitResult r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);                     // does not compile
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(
        r, "a borrow may not be returned through the owning-type return"));
}
