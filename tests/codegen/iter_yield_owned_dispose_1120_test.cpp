// Codegen — §29 (#1120): an IEnumerable<T> iterator that yields OWNED class
// values must dispose the yielded value on each MoveNext advance (before
// storing the next) and on enumerator Dispose (break / exhaustion).
//
// Spec authority: §29 Iterator yield ownership (v0.155):
//   "the enumerator owns the most-recently-yielded value; the next MoveNext
//    disposes the previous before producing the next, and Dispose disposes the
//    outstanding yielded value (break / exhaustion / Dispose)."
//
// Root cause (pre-fix): LlvmBackend_Iterators.cpp emitIteratorMethod never
// disposed the current_val frame field — neither in the MoveNext body before
// overwriting it, nor in Dispose or on normal exhaustion.  Every yielded owned
// class object silently leaked.
//
// Fix:
//   - MoveNext body (LlvmBackend_Stmts.cpp yield-return handler): null-guard
//     the previous current_val and emitDisposeAndReclaim before storing the new
//     value.  No-op on the first yield (calloc-zeroed null).
//   - Normal exhaustion path (LlvmBackend_Iterators.cpp after lowerBody):
//     dispose current_val, null the slot, then set state=-1 and return false.
//     This makes the subsequent for-loop Dispose a no-op (null-guard).
//   - Dispose / break path (emitDisposeBody): disposeYieldedVal lambda,
//     null-guard the current_val field, runs for both the source and clone
//     Dispose functions, before disposeOwnedParams and free(self).
//
// Tests verify disposal counts with a Cell.disposed static counter (ctor sets
// disposed=0 at start of Main, dtor increments disposed).  Expected count after
// the loop: exactly one per unique Cell instance produced (no leak, no
// double-free).

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
static const std::filesystem::path& wtDir1120() {
    return wtScratchDir("wt-1120");
}

// Compile `source` to a native executable and run it; returns the exit code,
// or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir1120();
    std::string stem = unique_stem("bezel_1120");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
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

// Cell with a static disposal counter.  Destructor increments Cell.disposed.
// Main resets it to 0 at entry so the count is test-local.
const char* kCellDisposed =
    "class Cell "
    "  public static disposed: int := 0; "
    "  public n: int; "
    "  public constructor(v: int) n := v; end constructor; "
    "  destructor Cell.disposed := Cell.disposed + 1; end destructor; "
    "end Cell; ";

}  // namespace

// §29 (#1120, single-yield): the one Cell yielded must be disposed exactly
// once when the for-loop completes normally.  sum=100, disposed=1 → rc=101.
BEZEL_TEST(iter_yield_owned_dispose_1120, single_yield_disposed_on_exhaustion) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kCellDisposed +
        "class C "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(100); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Cell.disposed := 0; "
        "    var sum := 0; "
        "    for var c in Gen() do sum := sum + c.n; end; "
        "    return sum + Cell.disposed; "  // 100 + 1 = 101
        "  end Main; end C;"), 101);
}

// §29 (#1120, 3-element): each of three Cells must be disposed exactly once
// in turn.  sum=1+2+3=6, disposed=3; rc = 6*10 + 3 = 63.
BEZEL_TEST(iter_yield_owned_dispose_1120, three_yields_each_disposed_once) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kCellDisposed +
        "class C "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(1); "
        "    yield return new Cell(2); "
        "    yield return new Cell(3); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Cell.disposed := 0; "
        "    var sum := 0; "
        "    for var c in Gen() do sum := sum + c.n; end; "
        "    return sum * 10 + Cell.disposed; "  // 60 + 3 = 63
        "  end Main; end C;"), 63);
}

// §29 (#1120, break): consumer breaks after 1 element; the outstanding yielded
// Cell(1) must be disposed at enumerator-Dispose.  Cell(2) and Cell(3) are
// never produced.  sum=1, disposed=1 → rc=11.
BEZEL_TEST(iter_yield_owned_dispose_1120, break_disposes_outstanding_value) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kCellDisposed +
        "class C "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(1); "
        "    yield return new Cell(2); "
        "    yield return new Cell(3); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Cell.disposed := 0; "
        "    var sum := 0; "
        "    var count := 0; "
        "    for var c in Gen() do "
        "      sum := sum + c.n; "
        "      count := count + 1; "
        "      if count = 1 then break; end; "
        "    end; "
        "    return sum * 10 + Cell.disposed; "  // 10 + 1 = 11
        "  end Main; end C;"), 11);
}

// §29 (#1120, break after 2nd): consumer breaks after 2nd element; Cell(1)
// was already disposed by MoveNext#2 (before yielding Cell(2)); Cell(2) is
// disposed by enumerator-Dispose on break.  disposed=2, sum=1+2=3 → rc=32.
BEZEL_TEST(iter_yield_owned_dispose_1120, break_after_second_disposes_both) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kCellDisposed +
        "class C "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(1); "
        "    yield return new Cell(2); "
        "    yield return new Cell(3); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Cell.disposed := 0; "
        "    var sum := 0; "
        "    var count := 0; "
        "    for var c in Gen() do "
        "      sum := sum + c.n; "
        "      count := count + 1; "
        "      if count = 2 then break; end; "
        "    end; "
        "    return sum * 10 + Cell.disposed; "  // 30 + 2 = 32
        "  end Main; end C;"), 32);
}

// §29 (#1120, non-owned regression): a non-owned int iterator is unaffected
// by the fix.  Verifies the elemIsOwnedClass guard prevents spurious disposal
// on value-type iterators.  sum = 10+20+30 = 60 → rc=60.
BEZEL_TEST(iter_yield_owned_dispose_1120, non_owned_int_iterator_unaffected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 10; yield return 20; yield return 30; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in Gen() do sum := sum + x; end; "
        "    return sum; "  // 60
        "  end Main; end C;"), 60);
}

// §29 (#1120, re-enumeration): the same IEnumerable<Cell> source iterated
// twice (§29.11 GetEnumerator clone).  Each clone disposes its own yielded
// values independently.  Two loops over a single-yield iterator → disposed=2.
// sum from both loops = 100+100=200, disposed=2 → rc=202.
BEZEL_TEST(iter_yield_owned_dispose_1120, reenumeration_each_clone_disposes) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M; ") + kCellDisposed +
        "class C "
        "  public static method Gen(): IEnumerable<Cell> "
        "    yield return new Cell(100); "
        "  end Gen; "
        "  public static method Main(): int "
        "    Cell.disposed := 0; "
        "    var sum := 0; "
        "    var src := Gen(); "
        "    for var c in src do sum := sum + c.n; end; "
        "    for var c in src do sum := sum + c.n; end; "
        "    return sum + Cell.disposed; "  // 200 + 2 = 202
        "  end Main; end C;"), 202);
}
