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
//
// ── HARNESS (test #6386) ───────────────────────────────────────────────────
//
// This file used to carry TWO private runners: `compileAndRunCrossModule` (four
// hand-rolled steps over a `compileObject`'d library plus an importer, with its
// own `TempFile`/`writeSource`/exit-code decoding) and
// `compileAndRunSingleModule`. Both are now the shared helpers from
// tests/support/aot_support.hpp (#6379):
//
//   * the three cross-module rows use `bezel::test::runAotModules`, whose run
//     half is `aot_detail::runProgramFill` — the SAME function `runAot` calls —
//     and whose compile half is `forkedCompileModule` (a `Driver::compileObject`,
//     the only shape that embeds the §2.4 `.bezel.meta` public interface) plus
//     `forkedCompileWithDeps`;
//   * the CONTROL row is a SINGLE-module program, and #6379 is explicit that
//     `runAotModules` is not a replacement for `runAot` — a one-module row keeps
//     the content-addressed link-cache that a whole gate collapses 2-3
//     compilations of the same program into 1. It is therefore `runAot`.
//
// WHAT THE PORT ADDS. Each row asserted a bare exit code, and the old runner
// returned -1 for a compile failure, a link failure, a fork failure and a
// runtime signal alike. Every row now asserts `r.ran` FIRST and
// unconditionally, so none of them can pass for a reason other than "the
// program compiled, linked, ran to normal termination and returned N".
//
// ⚠ ONE REAL DIFFERENCE, measured and deliberate. The old runner executed with
// `std::system(exe)`, which inherits the ambient environment — so under the
// #1822 whole-suite ASan sweep (`BEZEL_AOT_FORCE_SANITIZE=address`) it ran with
// LeakSanitizer ON by default, and a leak would have replaced the program's own
// exit status with LSan's. `runAot` pins `detect_leaks=0` under the sweep
// instead, because whole-suite leak detection is too noisy and because §28.2
// checked disposal leaks BY DESIGN (#4532 measured that cost). MEASURED under
// `BEZEL_AOT_FORCE_SANITIZE=address` at this commit: all four rows are clean,
// so no row here depended on that inherited leak detection.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// The dependency's MODULE NAME is a parameter, not a constant: these rows each
// name their library differently (`SeqLib`, `RangeLib`, `LimLib`), and the
// harness matches a module to its `import` by name — a wrong name here would
// make the driver resolve nothing and every row would report the same
// unrelated "module not found" reject. `App` is the entry in all three.
JitResult compileAndRunCrossModule(const std::string& libName,
                                   const std::string& libSrc,
                                   const std::string& mainSrc) {
    return runAotModules(
        {ModuleSource{libName, libSrc}, ModuleSource{"App", mainSrc}}, "App");
}

}  // namespace

// ---------------------------------------------------------------------------
// CONTROL — same-module iterator still works (non-regression for §29.3).
// ---------------------------------------------------------------------------

// §29.3 (control): same-module yield iterator sums 1..5 → 15.
// This must continue to work after the cross-module fix.
//
// SINGLE-MODULE, so `runAot` and not `runAotModules` — see the harness note
// above. Its value assertion is unchanged: still 15 (1+2+3+4+5), now preceded
// by `r.ran`.
BEZEL_TEST(cross_module_iter_495, control_same_module_iterator_spec_29_3) {
    JitResult r = runAot(
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
        "end Seq;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
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
//
// ⚠ THIS IS THE ROW THE PORT MAKES SHARPEST. The defect it pins was a SEGFAULT.
// Under the old runner a SIGSEGV surfaced as `status == -1` → `WIFEXITED` false
// → -1, which differs from 15, so the row did go red. But it was red for the
// same reason an unrelated toolchain hiccup would make it red, and the
// diagnostic said nothing about where. `r.ran` is false on a runtime signal
// and `r.diagnostics` then carries "program terminated by signal 11" — so a
// re-break reports the crash rather than a bare number mismatch.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_iterator_sum_spec_29_3) {
    JitResult r = compileAndRunCrossModule("SeqLib",
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
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);  // 1+2+3+4+5
}

// §29.3 / §2.5 (#495-A): cross-module yield iterator with a while loop body;
// verifies that the captured parameter (`n`) is correctly spilled/restored in
// the imported state machine frame.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_while_yield_spec_29_3) {
    JitResult r = compileAndRunCrossModule("RangeLib",
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
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);  // 1+2+…+10
}

// §29.3 / §2.5 (#495-A): cross-module iterator with a conditional yield break;
// verifies the vtable dispatch survives early termination from the imported
// iterator.
BEZEL_TEST(cross_module_iter_495, cross_module_sync_limited_yield_break_spec_29_3) {
    JitResult r = compileAndRunCrossModule("LimLib",
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
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);  // 1+2+3+4+5
}