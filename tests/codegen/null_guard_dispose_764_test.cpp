// fix #764 / §28.3 + §28.7: the null-guard idiom `if x <> null then x.Dispose()
// end` for an owned nullable local is now accepted. The ownership checker's CFG
// dataflow (Ownership.cpp computeEntry/foldPred + the CFGBlock::condition added
// in CFG.hpp) null-discriminates the branch successors: on the edge where x is
// provably null (the false edge of `x <> null`, the true edge of `x = null`), x
// is fed into the meet as Disposed — it holds null and disposal of null is a
// no-op (§28.3). The non-null edge keeps x Owned, so the fix is
// CONDITION-SENSITIVE.
//
// Historical note (#2270): the two soundness cells at the bottom of this file
// originally asserted that a conditional dispose on a NON-null-check condition,
// and a null branch that re-acquires ownership, stay REJECTED as genuine leaks.
// That is no longer the rule — §28.3's affine auto-drop and K7.4's "scope-end
// disposal per-path" accept both, disposing per-path — so those cells now assert
// the stronger exactly-once runtime property instead. See the note there.
//
// NOTE: these tests drive the REAL Driver pipeline (which runs the ownership
// pass) rather than the plain JIT harness — runJit does NOT run ownership
// analysis, so it cannot observe accept/reject of an ownership obligation.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir764() {
    return wtScratchDir("wt-764");
}

// Compile via the real Driver to a native exe and run it; exit code or -1 on
// compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir764();
    std::string stem = unique_stem("bezel_764");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// (A `compileFails` helper lived here for the two negative cells below. Both
// were rewritten to positive compileAndRun assertions for #2270 — see the note
// at those cells — leaving it unused, so it was removed rather than left to
// draw -Wunused-function.)

const char* widget =
    "module M; "
    "class Widget "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "  public static method MaybeNew(b: bool): owned Widget? "
    "    if b then return new Widget(); end; "
    "    return null; "
    "  end MaybeNew; "
    "end Widget; ";
}  // namespace

// §28.7/#764: the issue's exact repro — was rejected ('owned value would be
// dropped without disposal'); now compiles and disposes once (Widget._n == 1).
BEZEL_TEST(null_guard_dispose_764, null_guard_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  var x := Widget.MaybeNew(true); "
        "  if x <> null then x.Dispose(); end; "
        "  return Widget._n; "  // 1
        "end Main; end C;"), 1);
}

// §28.7: the reversed-operand form `null <> x` is recognised too.
BEZEL_TEST(null_guard_dispose_764, reversed_operand_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  var x := Widget.MaybeNew(true); "
        "  if null <> x then x.Dispose(); end; "
        "  return Widget._n; "  // 1
        "end Main; end C;"), 1);
}

// §28.7: when x is null at runtime, the guard skips and nothing is disposed.
BEZEL_TEST(null_guard_dispose_764, guard_when_null_at_runtime) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  var x := Widget.MaybeNew(false); "  // null
        "  if x <> null then x.Dispose(); end; "
        "  return Widget._n; "  // 0
        "end Main; end C;"), 0);
}

// §28.3 v0.150 (#1008 Part B): a null-guard whose then-branch never disposes no
// longer leaks — the undischarged plain-`var` `x` AUTO-DROPS at scope end
// (must-use warning), disposing the owned (non-null) value exactly once. The
// owned local is placed in an inner `do` block so the auto-drop runs before
// `Widget._n` is read (Widget._n becomes 1; null-guarded disposal is exactly-once).
BEZEL_TEST(null_guard_dispose_764, null_guard_without_dispose_auto_dropped) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  do "
        "    var x := Widget.MaybeNew(true); "
        "    if x <> null then Widget._n := Widget._n + 0; end; "  // no dispose
        "  end; "                                                  // x auto-drops
        "  return Widget._n; "                                     // 1
        "end Main; end C;"), 1);
}

// ── REWRITTEN 2026-07-29 for issue #2270 (behaviour change in d2ecf82) ──────
//
// The two cells below asserted that a conditional dispose leaving `x` owned on
// the other edge is a REJECTED leak. That encoded the pre-auto-drop rule and is
// stale. Under spec v0.228 both shapes are K7.4 maybe-consumed joins with NO
// post-join use of `x`, and K7.4 (kernel join rule 4; restated at §28.3)
// resolves them: "owned ⊓ moved-from and owned ⊓ explicitly-disposed =
// maybe-consumed — unreadable after the join, no flow around a back edge into a
// use; scope-end disposal per-path." The still-owned path's obligation is met by
// §28.3's affine auto-drop, which is "a warning (not an error)". The disposed
// path is skipped because an explicit `Dispose()` nulls the slot.
//
// Both are therefore rewritten from compileFails() to compileAndRun(), which is
// the stronger assertion the original cells were reaching for: they now pin
// that the per-path drop is EXACTLY ONCE, not merely that the program compiles.
// As in `null_guard_without_dispose_auto_dropped` above, the owned local sits in
// an inner `do` block so its auto-drop runs BEFORE `Widget._n` is read (the
// §22.x return edge otherwise computes the return value before scope-end
// disposals, which would hide a missing drop).

// §28.3 / K7.4: a null branch that RE-ACQUIRES ownership. `MaybeNew(false)`
// makes the else arm the one that runs, so the reacquired Widget is the value
// under test: the join is Disposed (then) ⊓ Owned (else, fresh) = maybe-consumed,
// there is no post-join use, and the fresh value is auto-dropped at the `do`
// block's end. Widget._n == 1 proves the reacquired value is disposed exactly
// once (0 would mean the reassignment lost the obligation; 2 a double free).
BEZEL_TEST(null_guard_dispose_764, null_branch_reacquire_auto_dropped_k7_4) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  do "
        "    var x := Widget.MaybeNew(false); "  // null → the else arm runs
        "    if x <> null then x.Dispose(); else x := new Widget(); end; "
        "  end; "                                // per-path auto-drop of the fresh x
        "  return Widget._n; "                   // 1
        "end Main; end C;"), 1);
}

// §28.3 / K7.4: a conditional dispose on a NON-null-check condition (a
// non-nullable owned local + a bool flag). This is the plain maybe-consumed
// join — no null-guard discrimination involved — and it is accepted the same
// way: the then arm disposes, the fall-through arm auto-drops, and the
// null-guarded scope-end drop makes it exactly once either way.
// Widget._n == 1 (2 would be the double free the old rejection feared).
BEZEL_TEST(null_guard_dispose_764, non_null_check_conditional_dispose_auto_dropped_k7_4) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string(widget) +
        "class C public static method Main(): int "
        "  do "
        "    var flag := true; "
        "    var x: Widget := new Widget(); "
        "    if flag then x.Dispose(); end; "  // disposed here; drop skips at scope end
        "  end; "
        "  return Widget._n; "                 // 1
        "end Main; end C;"), 1);
}
