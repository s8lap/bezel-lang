// Codegen + runtime — LINQ operator closure-cell reference counting (issue #422,
// closures increment; §28.9 runtime-managed reclamation; §28.3 closures borrow
// their captures).
//
// A LINQ operator that takes a predicate/selector closure receives, when that
// argument is an INLINE LAMBDA (a lambda literal), a freshly-allocated transient
// closure cell { fn, env, rc } with rc=1 — a +1 producer reference the call
// site owns. Before this change that transient was NEVER released: every call
// leaked the cell (and, when capturing, its env block). Two operator families
// must be handled differently:
//
//   * EAGER operators (Any/All/Count/First/Sum/Average/Min/Max/Aggregate): the
//     closure is consumed entirely in a loop that completes before the call
//     returns. The inline-lambda transient is released at the loop-end, at the
//     call site (the same shape as the Dictionary.GetOrAdd factory release).
//
//   * LAZY operators (Where/Select/OrderBy/TakeWhile/SkipWhile/SelectMany/Zip):
//     the closure is stored INTO the lazy enumerator (bz_lazy_t::closure) and
//     invoked LATER during iteration. Releasing it at the call site would free
//     it before iteration → use-after-free. Instead codegen marks the
//     enumerator (bz_lazy_own_closure) so bz_lazy_dispose releases the closure
//     when the lazy chain is disposed — alongside the existing up/down/inner
//     frees. (Take/Skip/Concat/Distinct carry no closure and are untouched.)
//
// The guard in BOTH families: release ONLY the inline-lambda form. A closure
// arriving via a variable/field is a BORROW the operator does not own; freeing
// it would be a double-free / use-after-free. Captured values are themselves
// borrows (§28.3), so releasing the cell frees only the cell + env, never a
// capture — no double-dispose.
//
// Two checks, mirroring closure_refcount_test.cpp:
//   1. expectValueAndLeakFree: native exit code proves correctness AND, under
//      LeakSanitizer (re-lowered + linked against an ASan-built runtime), proves
//      the transient is reclaimed (no leak) AND memory-safe (no UAF/double-free).
//   2. expectValueAndNoCorruption: tolerates a leak but requires no memory-safety
//      error — the borrowed-closure regression guard (we must NOT free a borrow).
//
// Both legs SKIP gracefully when no clang++ toolchain is present.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_422linq_" + std::to_string(counter++);
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

// Lower `source` to an object via the real pipeline; -1 on any failure (SKIP).
int lowerToObject(const std::string& source, const std::string& objPath) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -1;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -1;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return -1;
    std::string err;
    if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    return 0;
}

// LeakSanitizer verification. Returns 0 (clean: no leak, no UAF/DF),
// 1 (sanitizer reported a problem), or -1 (toolchain unavailable: SKIP).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_422linq_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return -1;
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Correctness + LSan-clean (no leak, no UAF/double-free).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean
}

// Run under ASan with detect_leaks=0: catches every memory-safety corruption
// (UAF / double-free → exit 99) but TOLERATES a leak. Returns true iff the
// program exited with `expectedExitCode` (no corruption), or the toolchain is
// unavailable (SKIP). Used to run a lazy-materialized program multiple times to
// prove the dispose-path closure release never produces a UAF.
// Uses the memoized ASan runtime object (compiled once per process).
bool runNoCorruption(const std::string& source, int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return true;  // toolchain unavailable: SKIP
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_422linq_safe_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return true;  // SKIP
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return true; }
    // exitcode=99 is ASan's distinct error code (so a UAF can never be confused
    // with the program legitimately returning expectedExitCode). No test expects
    // 99. detect_leaks=0 permits the deliberate borrow leak but still catches a
    // UAF / double-free.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return true;  // SKIP
    return WIFEXITED(status) && WEXITSTATUS(status) == expectedExitCode;
}

void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    BEZEL_EXPECT_TRUE(runNoCorruption(source, expectedExitCode));
}

// --- Programs ------------------------------------------------------------

// EAGER operator (§5 Any), inline lambda called MANY times in a loop. Before
// this change each Any() leaked the inline-predicate cell; after, it is released
// at the loop-end. 200000 iterations ⇒ a large, unmistakable leak if unfixed;
// LSan-clean after. The predicate is non-capturing (a `{ fn, null env, rc }`
// cell). Returns 0 (every iteration's Any is true).
const char* kEagerAnyChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 200000 do "
    "      if not a.Any((x) => x > 3) then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// EAGER operator (§5 Count) with a CAPTURING inline lambda churned in a loop.
// The lambda captures `threshold` (a value-typed local → boxed into a shared
// cell, plus the closure cell { fn, env, rc } and its env block). The closure
// cell + env block are released at the loop-end; the boxed capture cell is an
// orthogonal value-box leak (not a closure cell — outside this fix's scope), so
// this case is checked for CORRECTNESS + NO-CORRUPTION (not LSan-clean). Returns
// 0 (Count is 2 each iteration: 4 and 5 exceed 3).
const char* kEagerCountCapturingChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
    "    var threshold: int := 3; "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      if a.Count((x) => x > threshold) <> 2 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// EAGER Aggregate (binary fold) with an inline lambda churned in a loop. The
// fold cell is released at the loop-end. Returns 0 (sum = 15 each iteration).
const char* kEagerAggregateChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      var s: int := a.Aggregate(0, (acc: int, x: int) => acc + x); "
    "      if s <> 15 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// LAZY Where with an inline lambda, MATERIALIZED via ToArray() and churned in a
// loop. The predicate cell is stored into the lazy enumerator and released when
// bz_lazy_dispose runs (ToArray drains + disposes the pipeline). The materialized
// array is `using`-disposed each iteration. With dispose-release wired, this is
// both UAF-free AND LSan-clean. Returns 0 (each ToArray yields {2,4}).
const char* kLazyWhereToArrayChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      using var e: Array<int> := a.Where((x) => x mod 2 = 0).ToArray(); "
    "      if e.Length <> 2 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// LAZY Select with an inline lambda, MATERIALIZED + churned. The selector cell
// is released at enumerator dispose. UAF-free and LSan-clean. Returns 0.
const char* kLazySelectToArrayChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      using var e: Array<int> := a.Select((x) => x * 10).ToArray(); "
    "      if e.Get(2) <> 30 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// LAZY OrderBy with an inline key selector, MATERIALIZED + churned. Releases the
// key-selector cell at dispose. UAF-free and LSan-clean. Returns 0.
const char* kLazyOrderByToArrayChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(5); a.Add(2); a.Add(8); a.Add(1); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      using var e: Array<int> := a.OrderBy((x: int) => x).ToArray(); "
    "      if e.Get(0) <> 1 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// LAZY Where, FULLY ITERATED in a for-loop then dropped (NO ToArray). The
// for-loop's enumerator dispose path releases the stored predicate cell. This is
// the regression guard: prove iterate-then-drop of a release-on-dispose closure
// does NOT produce a UAF. Churned to stress the dispose path. Returns 0
// (sum = 2 + 4 = 6 each iteration).
const char* kLazyWhereForLoopChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      var s: int := 0; "
    "      for var x in a.Where((x) => x mod 2 = 0) do s := s + x; end; "
    "      if s <> 6 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// BORROW guard (EAGER): the predicate is a VARIABLE (bound once, reused), NOT an
// inline lambda — a borrow the operator does not own. Codegen must NOT release
// it (own_closure stays 0 / the eager guard is false), else the second use would
// be a UAF / double-free. The closure local IS released exactly once at its own
// scope end (the §28.9 non-escaping-local path). Correct value + NO corruption.
// (May leak nothing extra here; the point is the absence of a double-free.)
const char* kBorrowedPredicateEagerReuse =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
    "    var pred: (int) -> bool := (x: int) => x > 2; "
    "    var c1: int := a.Count(pred); "
    "    var c2: int := a.Count(pred); "
    "    if c1 = 2 and c2 = 2 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// LAZY Zip with an inline COMBINE lambda (args[1]) AND a nested inline Where on
// the second source. Exercises two owned closures in one chain: the combine cell
// on the outer Zip node and the predicate cell on the `down` enumerator — each
// released at its OWN node's dispose via the cascade. Materialized + churned.
// UAF-free and LSan-clean. b.Where(y>10) = {20,30,40}; Zip with a={1,2,3} pairs
// 3, so result length 3. Returns 0.
const char* kLazyZipNestedWhereToArrayChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); "
    "    using var b := new Array<int>(); "
    "    b.Add(10); b.Add(20); b.Add(30); b.Add(40); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 50000 do "
    "      using var z: Array<int> := "
    "        a.Zip(b.Where((y) => y > 10), (x: int, y: int) => x + y).ToArray(); "
    "      if z.Length <> 3 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// BORROW guard (LAZY): a VARIABLE predicate reused across two Where pipelines,
// each materialized. The enumerator must NOT release the borrowed closure on
// dispose (own_closure stays 0) — else the first pipeline's dispose would free
// the cell the second pipeline still references → UAF. Correct value + NO
// corruption. Returns 0.
const char* kBorrowedPredicateLazyReuse =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
    "    var pred: (int) -> bool := (x: int) => x > 2; "
    "    using var e1: Array<int> := a.Where(pred).ToArray(); "
    "    using var e2: Array<int> := a.Where(pred).ToArray(); "
    "    if e1.Length = 2 and e2.Length = 2 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// FINDING [5] / stdlib §5: an EAGER terminal (Count) applied DIRECTLY to a LAZY
// pipeline (Where with an inline predicate) is now correct — the terminal
// MATERIALIZES the pipeline (bz_lazy_to_array), folds the snapshot, then frees
// the snapshot array (bz_array_free, no element disposal). bz_lazy_to_array also
// disposes the enumerator, releasing its inline-lambda predicate cell. Churned
// 100000× ⇒ a large unmistakable leak if either the materialized array or the
// predicate cell were not freed; LSan-clean after the fix. Where(x>1) over
// {1,2,3,4} yields {2,3,4}, so Count() == 3. Returns 0 on match.
const char* kLazyWhereThenCountChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      if a.Where((x) => x > 1).Count() <> 3 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// FINDING [5]: a CHAINED lazy pipeline (Where then Select, two inline closures)
// consumed by an eager terminal (Aggregate, its own inline fold). The
// materialized snapshot array is freed at the terminal return; the Where and
// Select inline-lambda cells are released at bz_lazy_dispose during
// materialization; the eager fold cell is released at the terminal loop-end.
// Churned 50000×; LSan-clean. Where(x>1)={2,3,4}; Select(*10)={20,30,40};
// Aggregate(+) = 90. Returns 0 on match.
const char* kLazyWhereSelectThenAggregateChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); "
    "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 50000 do "
    "      var s: int := a.Where((x) => x > 1).Select((x) => x * 10)"
    "                     .Aggregate(0, (acc: int, x: int) => acc + x); "
    "      if s <> 90 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// --- EAGER: inline-lambda transient released at the call-site loop-end ---

BEZEL_TEST(linq_refcount, eager_any_inline_lambda_churn_leak_free_spec_28_9) {
    expectValueAndLeakFree(kEagerAnyChurn, 0);
}

BEZEL_TEST(linq_refcount, eager_count_capturing_inline_lambda_no_corruption_spec_28_9) {
    // Closure cell + env released; the orthogonal value-box capture leak is out
    // of scope, so this is a no-corruption (not leak-free) check.
    expectValueAndNoCorruption(kEagerCountCapturingChurn, 0);
}

BEZEL_TEST(linq_refcount, eager_aggregate_inline_lambda_churn_leak_free_spec_28_9) {
    expectValueAndLeakFree(kEagerAggregateChurn, 0);
}

// --- LAZY: inline-lambda closure released at enumerator dispose ---

BEZEL_TEST(linq_refcount, lazy_where_toarray_inline_lambda_leak_free_spec_28_9) {
    expectValueAndLeakFree(kLazyWhereToArrayChurn, 0);
}

BEZEL_TEST(linq_refcount, lazy_select_toarray_inline_lambda_leak_free_spec_28_9) {
    expectValueAndLeakFree(kLazySelectToArrayChurn, 0);
}

BEZEL_TEST(linq_refcount, lazy_orderby_toarray_inline_lambda_leak_free_spec_28_9) {
    expectValueAndLeakFree(kLazyOrderByToArrayChurn, 0);
}

BEZEL_TEST(linq_refcount, lazy_zip_nested_where_two_owned_closures_leak_free_spec_28_9) {
    expectValueAndLeakFree(kLazyZipNestedWhereToArrayChurn, 0);
}

// --- FINDING [5]: eager terminal applied DIRECTLY to a lazy pipeline. The
//     terminal materializes the pipeline, folds, and frees the snapshot array;
//     the lazy enumerator's inline-lambda closures are released at its dispose.
//     Both must be leak-free; run the basic case 3× to stress the path. ---

BEZEL_TEST(linq_refcount, lazy_where_then_count_terminal_leak_free_spec_5) {
    BEZEL_EXPECT_EQ(compileAndRun(kLazyWhereThenCountChurn), 0);
    for (int rep = 0; rep < 3; ++rep) {
        int lsan = runUnderLeakSanitizer(kLazyWhereThenCountChurn);
        if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
    }
}

BEZEL_TEST(linq_refcount, lazy_where_select_then_aggregate_terminal_leak_free_spec_5) {
    expectValueAndLeakFree(kLazyWhereSelectThenAggregateChurn, 0);
}

// --- LAZY regression guard: iterate-then-drop of a release-on-dispose closure
//     must not UAF. Run THREE times to stress the dispose path. ---

BEZEL_TEST(linq_refcount, lazy_where_forloop_drop_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(compileAndRun(kLazyWhereForLoopChurn), 0);
    for (int rep = 0; rep < 3; ++rep)
        BEZEL_EXPECT_TRUE(runNoCorruption(kLazyWhereForLoopChurn, 0));
    // Also assert LSan-clean: dispose-release reclaims the stored predicate cell.
    int lsan = runUnderLeakSanitizer(kLazyWhereForLoopChurn);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// --- BORROW guards: a variable-form closure is NOT owned; never freed by the
//     operator (cardinal anti-UAF check). Run the materialized lazy case 3×. ---

BEZEL_TEST(linq_refcount, eager_borrowed_predicate_reused_no_double_free_spec_28_3) {
    expectValueAndNoCorruption(kBorrowedPredicateEagerReuse, 0);
}

BEZEL_TEST(linq_refcount, lazy_borrowed_predicate_reused_no_uaf_spec_28_3) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowedPredicateLazyReuse), 0);
    for (int rep = 0; rep < 3; ++rep)
        BEZEL_EXPECT_TRUE(runNoCorruption(kBorrowedPredicateLazyReuse, 0));
}

// ---- stdlib §5 gaps milestone: ThenBy inline-lambda closure release ----------
//
// ThenBy stores a secondary key closure into the THENBY enumerator node. When
// the closure is an INLINE LAMBDA, codegen calls bz_lazy_own_closure2, which
// sets own_closure2 := 1 so bz_lazy_dispose releases it. Churned 100000× to
// detect any leak (closure cell + env are released on dispose). Returns 0
// (composite sort is correct and the result Count is 4). LSan-clean after.

const char* kLazyThenByToArrayChurn =
    "module M; record Point(x: int, y: int); "
    "class C "
    "  public static method Main(): int "
    "    using var a := new Array<Point>(); "
    "    a.Add(new Point(2, 4)); a.Add(new Point(1, 5)); "
    "    a.Add(new Point(2, 1)); a.Add(new Point(1, 3)); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      using var r: Array<Point> := a.OrderBy((p) => p.x)"
    "                                    .ThenBy((p) => p.y)"
    "                                    .ToArray(); "
    "      if r.Length <> 4 then ok := 0; end; "
    "      if r.Get(0).y <> 3 then ok := 0; end; "  // (1,3) first
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, lazy_thenby_toarray_inline_lambda_leak_free_spec_28_9) {
    expectValueAndLeakFree(kLazyThenByToArrayChurn, 0);
}

// ThenBy with a VARIABLE secondary key closure (borrowed) — must NOT release
// on dispose, else the second iteration causes a UAF. Returns 0 (correct sort,
// no corruption).
const char* kLazyThenByBorrowedKey =
    "module M; record Point(x: int, y: int); "
    "class C "
    "  public static method Main(): int "
    "    using var a := new Array<Point>(); "
    "    a.Add(new Point(2, 4)); a.Add(new Point(1, 5)); "
    "    a.Add(new Point(2, 1)); a.Add(new Point(1, 3)); "
    "    var secKey: (Point) -> int := (p: Point) => p.y; "
    "    using var r1: Array<Point> := a.OrderBy((p) => p.x)"
    "                                   .ThenBy(secKey).ToArray(); "
    "    using var r2: Array<Point> := a.OrderBy((p) => p.x)"
    "                                   .ThenBy(secKey).ToArray(); "
    "    if r1.Length = 4 and r2.Length = 4 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, lazy_thenby_borrowed_key_no_uaf_spec_28_3) {
    BEZEL_EXPECT_EQ(compileAndRun(kLazyThenByBorrowedKey), 0);
    for (int rep = 0; rep < 3; ++rep)
        BEZEL_EXPECT_TRUE(runNoCorruption(kLazyThenByBorrowedKey, 0));
}

// Union with Where: the `other` enumerator is consumed at construction (no
// closure per se, but the dict is heap-allocated → must be freed at dispose).
// Churned 100000× for leak detection. Returns 0 ({1,2,3,4} union = 4 elements).
const char* kLazyUnionToArrayChurn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
    "    using var b := new Array<int>(); b.Add(2); b.Add(3); b.Add(4); "
    "    var i: int := 0; var ok: int := 1; "
    "    while i < 100000 do "
    "      using var r: Array<int> := a.Union(b).ToArray(); "
    "      if r.Length <> 4 then ok := 0; end; "
    "      i := i + 1; "
    "    end; "
    "    if ok = 1 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, lazy_union_toarray_leak_free_spec_5) {
    expectValueAndLeakFree(kLazyUnionToArrayChurn, 0);
}
