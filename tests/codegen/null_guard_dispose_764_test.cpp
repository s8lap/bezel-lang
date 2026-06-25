// fix #764 / §28.3 + §28.7: the null-guard idiom `if x <> null then x.Dispose()
// end` for an owned nullable local is now accepted. The ownership checker's CFG
// dataflow (Ownership.cpp computeEntry/foldPred + the CFGBlock::condition added
// in CFG.hpp) null-discriminates the branch successors: on the edge where x is
// provably null (the false edge of `x <> null`, the true edge of `x = null`), x
// is fed into the meet as Disposed — it holds null and disposal of null is a
// no-op (§28.3). The non-null edge keeps x Owned, so the fix is
// CONDITION-SENSITIVE and SOUND: a conditional dispose on a non-null-check
// condition, and a null branch that re-acquires ownership, are still rejected as
// genuine leaks.
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
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Compile via the real Driver to a native exe and run it; exit code or -1 on
// compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_764_" + std::to_string(counter++);
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

// True when the source FAILS to compile (a §28.3 ownership-leak diagnostic).
bool compileFails(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_764_neg_" + std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

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

// §28.3 SOUNDNESS: a null branch that re-acquires ownership leaves x owned after
// the if; never disposing it is a real leak and must stay rejected (confirms the
// null-edge Disposed mark is overridden by the reassignment in the else body).
BEZEL_TEST(null_guard_dispose_764, null_branch_reacquire_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(widget) +
        "class C public static method Main(): int "
        "  var x := Widget.MaybeNew(true); "
        "  if x <> null then x.Dispose(); else x := new Widget(); end; "
        "  return Widget._n; "  // x owned from the else, never disposed → leak
        "end Main; end C;"));
}

// §28.3 SOUNDNESS: a conditional dispose on a NON-null-check condition (here a
// non-nullable owned local + a bool flag) is still a leak and must stay rejected
// — the relaxation fires only for a real null check, not an arbitrary condition.
BEZEL_TEST(null_guard_dispose_764, non_null_check_conditional_dispose_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        std::string(widget) +
        "class C public static method Main(): int "
        "  var flag := true; "
        "  using var keep: Widget := new Widget(); "  // anchor so the program is otherwise valid
        "  var x: Widget := new Widget(); "
        "  if flag then x.Dispose(); end; "  // not a null-check → leak when flag is false
        "  return Widget._n; "
        "end Main; end C;"));
}
