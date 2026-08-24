// Cross-module iterator tests (§29.2 / §29.3 / §2.5, issue #495).
//
// A `yield`-based iterator method compiles to a compiler-generated
// state-machine class with a vtable for MoveNext/Current/Dispose (§29.3).
// When the method is defined in a separately-compiled library and consumed
// cross-module, the importer reconstructs the method signature from the
// `.bezel.meta` section. The reconstructed return-type TypeRef for
// `IEnumerable<T>` has no AST node and no local declaration (it is a builtin
// interface, not a user-exported type), so codegen's `lowerTypeRef` must
// recognise it correctly and return a pointer-sized type — NOT the i32 default
// that caused the runtime SEGFAULT (bug A, §2.5 / §29.3).
//
// Two scenarios are tested:
//   A (sync)  — lib exports a `yield return`-based `Sequence(n): IEnumerable<int>`;
//               importer does `for var x in Lib.Sequence(5) do sum += x end`.
//               Before the fix: SEGFAULT at runtime (vtable pointer truncated to i32).
//               After: exits 15 (1+2+3+4+5).
//   CONTROL   — same-module iterator still works (non-regression).
//
// Bug B (async IAsyncEnumerable) is deferred: the fix for bug A (pointer-type
// lowering of the imported return TypeRef) is a prerequisite, but the async
// path requires additional investigation into the async state-machine frame
// lowering cross-module (the SIGABRT is in IR verification after the wrong
// load type is emitted).  Deferred per CLAUDE.md §4 decline-with-reasons.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's _lib.bzl / _lib.o /
// _main.bzl mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT).
const std::filesystem::path& wtDir495() {
    return wtScratchDir("wt-495");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib → object, compile main importing lib → executable, run;
// return exit code or -1.
int compileAndRunCrossModule(const std::string& libSrc,
                             const std::string& mainSrc) {
    auto dir = wtDir495();
    std::string stem = unique_stem("bezel_iter495");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / (stem + "_main")).string());

    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rm.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module helper (reuse iterator_native_test harness pattern).
int compileAndRunSingleModule(const std::string& src) {
    auto dir = wtDir495();
    std::string stem = unique_stem("bezel_iter495_sm");
    TempFile srcF((dir / (stem + ".bzl")).string());
    TempFile exeF((dir / stem).string());

    writeSource(srcF.path, src);

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(srcF.path, exeF.path);
    if (!r.success) return -1;

    int status = std::system(exeF.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ---------------------------------------------------------------------------
// CONTROL — same-module iterator still works (non-regression for §29.3).
// ---------------------------------------------------------------------------

// §29.3 (control): same-module yield iterator sums 1..5 → 15.
// This must continue to work after the cross-module fix.
BEZEL_TEST(cross_module_iter_495, control_same_module_iterator_spec_29_3) {
    BEZEL_EXPECT_EQ(compileAndRunSingleModule(
        "module M; "
        "public class Seq "
        "  public static method Sequence(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Sequence; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in Seq.Sequence(5) do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end Seq;"),
        15);  // 1+2+3+4+5
}

// ---------------------------------------------------------------------------
// Bug A (sync IEnumerable<int> cross-module) — §29.3 / §2.5 / issue #495.
//
// Before the fix: `declareImportedFunctions` lowered the return TypeRef for
// `IEnumerable<int>` to i32 (the default for an unrecognised Named type with
// no astNode and no local declaration), so the vtable-backed iterator pointer
// was truncated → SEGFAULT.
// After: the return TypeRef lowers to ptrTy() (pointer), matching the same-
// module path; the for-in loop calls MoveNext/Current through the vtable and
// sums correctly.
// ---------------------------------------------------------------------------

// §29.3 / §2.5 (#495-A): cross-module yield iterator; sum 1..5 = 15.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_iterator_sum_spec_29_3) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(
        // Library: exports a yield-based Sequence method.
        "module SeqLib; "
        "public class Seq "
        "  public static method Sequence(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Sequence; "
        "end Seq;",
        // Importer: consumes Seq.Sequence cross-module.
        "module App; "
        "import SeqLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in Seq.Sequence(5) do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end Program;"),
        15);  // 1+2+3+4+5
}

// §29.3 / §2.5 (#495-A): cross-module yield iterator with a while loop body;
// verifies that the captured parameter (`n`) is correctly spilled/restored in
// the imported state machine frame.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_while_yield_spec_29_3) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(
        "module RangeLib; "
        "public class R "
        "  public static method Range(lo: int, hi: int): IEnumerable<int> "
        "    var i := lo; "
        "    while i <= hi do "
        "      yield return i; "
        "      i := i + 1; "
        "    end; "
        "  end Range; "
        "end R;",
        "module App; "
        "import RangeLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in R.Range(1, 10) do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end Program;"),
        55);  // 1+2+…+10
}

// §29.3 / §2.5 (#495-A): cross-module iterator with a conditional yield break;
// verifies the vtable dispatch survives early termination from the imported
// iterator.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_limited_yield_break_spec_29_3) {
    BEZEL_EXPECT_EQ(compileAndRunCrossModule(
        "module LimLib; "
        "public class L "
        "  public static method Limited(n: int): IEnumerable<int> "
        "    for var i in 1..100 do "
        "      if i > n then yield break; end; "
        "      yield return i; "
        "    end; "
        "  end Limited; "
        "end L;",
        "module App; "
        "import LimLib; "
        "public class Program "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in L.Limited(5) do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end Program;"),
        15);  // 1+2+3+4+5
}
