// fix #724 F1 / §28.8.1 / §28.3 / §17.x: `var s := move x` (x an owned class)
// transfers ownership of x to `s`, so `s` is an OWNED local subject to the
// standard §28.3 discharge rules. Previously the ownership pass classified the
// move expression's result as Borrowed (producerState fell through to the
// default), so `s` carried no disposal obligation — it was neither disposed at
// scope end nor diagnosed as an undischarged owned local (a silent leak).
//
// This file pins F1 (the remaining defect) and includes regression pins for the
// sibling cluster members F2 (§17.13 discard) and F3 (use-after-move), which were
// already fixed on main. Accept/reject ownership tests drive the real Driver
// (runJit does not run the ownership pass).

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
static const std::filesystem::path& wtDir724() {
    return wtScratchDir("wt-724");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir724();
    std::string stem = unique_stem("bezel_724");
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

bool compileFails(const std::string& source) {
    auto dir = wtDir724();
    std::string stem = unique_stem("bezel_724_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

// A class with a destructor that bumps a static counter, so disposal is
// observable through the program's exit code.
const char* rclass =
    "class R public static n: int := 0; "
    "  destructor R.n := R.n + 1; end destructor; end R; ";
}  // namespace

// #724 F1 / §28.3 v0.150 (#1008 Part B): `var s := move x` makes `s` an owned
// local. Left undischarged it now AUTO-DROPS at scope end (must-use warning) —
// the moved value is disposed exactly once (R.n becomes 1), not leaked and not
// double-freed. (Pre-v0.150 this was a hard discharge error.)
BEZEL_TEST(move_result_owned_724, undischarged_move_result_auto_dropped) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + rclass +
        "class C public static method Main(): int "
        "  do var x := new R(); var s := move x; end; "  // s auto-drops here
        "  return R.n; "                                  // 1 — disposed once
        "end Main; end C;"), 1);
}

// #724 F1: the move result is owned, so discharging it by RETURN is accepted.
// v0.201 (#2048): the fresh-producing factory is spelled `owned R` per the
// declaration-site static-return rule.
BEZEL_TEST(move_result_owned_724, move_result_returned_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + rclass +
        "class C "
        "  public static method Make(): owned R "
        "    var x := new R(); var s := move x; return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var r := Make(); return 0; "  // disposed by the caller
        "  end Main; end C;"), 0);
}

// #724 F1: `using var s := move x` is accepted and DISPOSES s at scope end — the
// destructor runs (R.n becomes 1), proving the move result carries the obligation.
BEZEL_TEST(move_result_owned_724, using_var_move_result_disposed) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + rclass +
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  do using var s := move x; end; "  // s disposed here → R.n = 1
        "  return R.n; "
        "end Main; end C;"), 1);
}

// §17.13 v0.152 (#1009): discarding a freshly-constructed owned value is now
// PERMITTED — `_ := new R()` disposes it at end-of-statement (R.n == 1 proves
// the destructor ran EXACTLY once, not zero=leak/twice=double-free). Pre-v0.150
// this was a §17.13 reject; auto-drop (§28.3) makes the obligation satisfiable,
// so the discard simply names the immediate-disposal point.
BEZEL_TEST(move_result_owned_724, discard_disposable_new_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + rclass +
        "class C public static method Main(): int "
        "  _ := new R(); return R.n; "    // disposed at end of statement → 1
        "end Main; end C;"), 1);
}

// #724 F3 (regression pin, already fixed): a use after `_ := move x` via a `var`
// binding is caught as a use-after-move.
BEZEL_TEST(move_result_owned_724, use_after_move_var_binding_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class R destructor end destructor; end R; "
        "class C public static method Main(): int "
        "  var x := new R(); _ := move x; "
        "  var t := x; _ := move t; "    // x already moved out → error
        "  return 0; "
        "end Main; end C;"));
}

// §28.3 v0.152 (#1008 Part B, owned-parameter auto-drop): a dropped `owned`
// parameter is disposed EXACTLY ONCE at method end. `rclass`'s destructor bumps
// R.n, so R.n == 1 proves the auto-drop ran once — not zero (a leak) and not
// twice (a double-free with the caller's moved-out value). The caller `move`s
// the value in and so does not dispose it.
BEZEL_TEST(move_result_owned_724, owned_param_auto_drop_disposes_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + rclass +
        "class C "
        "  public static method Take(owned r: R): int return 0; end Take; "
        "  public static method Main(): int "
        "    var x := new R(); var k := C.Take(move x); "  // auto-dropped in Take
        "    return R.n; "                                  // 1 — disposed once
        "  end Main; end C;"), 1);
}
