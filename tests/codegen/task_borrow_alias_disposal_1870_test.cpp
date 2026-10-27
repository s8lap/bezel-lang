// Codegen/runtime — issue #1870 (§28.3 v0.190 SB-01 "Calls mint, storage
// lends" + §30.5): a task-typed local that merely BORROWS the task frame must
// NOT register its OWN scope-end task disposal.
//
// Under v0.190 SB-01 a Task-family CALL MINTS an OWNED task frame at the call
// site ("calls mint"); a bare-identifier ALIAS of a task local, or a read of
// task-typed STORAGE, only BORROWS that frame ("storage lends") — the mint CALL
// remains the sole owner and disposes the single `bz_task_t*` exactly once.
//
// The defect: codegen's scope-end task disposal (LlvmBackend_Stmts, the `d.task`
// branch) registered a disposal for EVERY task-typed local purely by type. So a
// borrow ALIAS got its OWN `d.task` alongside the owner's — both fired
// `bz_task_fire_and_forget` -> `bz_task_free` on the SAME frame at scope end, a
// double-free / UAF that corrupts async continuation state. The observable
// symptom is a HANG (the corruption derails the scheduler before any ASan trap),
// so the oracle here is exit-code / run-timeout AND a destructor-print count
// (exactly once), not ASan alone.
//
// The crux #1786 admitted plain-var / alias owned-task-local awaits (making the
// class-task alias newly reachable); this alias double-free was the blocking
// residual a pre-gate review caught. The same double-free pre-existed on main
// for a VALUE-task alias (`Task<int>`), filed as #1870.
//
// Fix (LlvmBackend_Stmts.cpp / LlvmBackend_Setup.cpp): register `d.task` ONLY for
// a task local that OWNS the frame — one whose initializer is a mint CALL. A
// borrow source (IdentifierExpr alias / MemberAccessExpr / IndexExpr storage
// read) is skipped (taskLocalInitializerIsBorrowSource), mirroring producerState's
// Owned-vs-Borrow split so codegen and the ownership pass agree.
//
// Verification uses the `Cell` static-counter idiom (ctor ++, dtor --; exit code
// == Cell.n after the task's scope closes, so 0 == disposed exactly once, a leak
// leaves it 1, a double-dispose underflows / hangs) PLUS a destructor "DISP"
// print counted in stdout (exactly-once positively). // per §28.3 v0.190 SB-01,
// §30.5 (#1786 / #1870)

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
// Direct owner (regression guard): a genuine owner still disposes exactly once.
// ---------------------------------------------------------------------------
BEZEL_TEST(task_borrow_alias_disposal_1870,
           class_task_direct_owner_disposed_exactly_once_28_3_v0190) {
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
    BEZEL_EXPECT_TRUE(r.ran);              // compiled + ASan-clean (no UAF)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "DISP"), 1);  // exactly once
}

// ---------------------------------------------------------------------------
// The core bug: a borrow ALIAS of an owned task local must NOT double-dispose.
// `var t := Producer(); var t2 := t; var c := await t2;` — `t2` BORROWS the
// frame ("alias borrows"); only `t` (the mint CALL) owns and disposes it. WITHOUT
// the fix `t` and `t2` both register `d.task` -> double-free of the one frame ->
// the Cell is disposed twice (or the async state corrupts and the program
// HANGS): `run()` returns the -999 hang sentinel / a nonzero exit. WITH the fix:
// disposed exactly once, exit 0, one "DISP".
// ---------------------------------------------------------------------------
BEZEL_TEST(task_borrow_alias_disposal_1870,
           class_task_alias_not_double_disposed_28_3_v0190) {
    const std::string prog =
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method Producer(): Task<Cell> "
        "    return new Cell(); "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    do var t := Producer(); var t2 := t; var c := await t2; end; "
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
// #1870 as filed: the SAME double-free pre-existed on main for a VALUE-task
// (`Task<int>`) alias — no class result, but the FRAME is still double-freed,
// hanging the program. There is no Cell to count; the assertion is that it RUNS
// (does not hang) and yields the awaited value. WITHOUT the fix: hang -> -999.
// WITH the fix: 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(task_borrow_alias_disposal_1870,
           value_task_alias_not_double_freed_1870) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static async method AsyncProd(): Task<int> "
        "    return 7; "
        "  end AsyncProd; "
        "  public static async method Main(): Task<int> "
        "    var t := AsyncProd(); "
        "    var t2 := t; "
        "    var x := await t2; "
        "    return x; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(prog), 7);   // no hang (-999)

    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);              // ASan-clean: no double-free of frame
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
