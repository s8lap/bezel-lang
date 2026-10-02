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
#include "support/emit_object_support.hpp"
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir422linq() {
    return wtScratchDir("wt-422linq");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir422linq();
    std::string stem = unique_stem("bezel_422linq");
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
    if (!unit) return bezel::test::asanBuildFailure("parse");
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return bezel::test::asanBuildFailure("emitModule");
    std::string err;
    if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    return 0;
}

// LeakSanitizer verification. Returns 0 (clean: no leak, no UAF/DF),
// 1 (sanitizer reported a problem), or -1 (toolchain unavailable: SKIP).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed
    auto dir = wtDir422linq();
    std::string stem = unique_stem("bezel_422linq_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return bezel::test::asanBuildFailure("lower");
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
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
    auto dir = wtDir422linq();
    std::string stem = unique_stem("bezel_422linq_safe");
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
    "      if not a.Any(lambda(x) => x > 3) then ok := 0; end; "
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
    // fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): composed spelling
    // `Where(pred).Count()` reaches the §5 operator pipeline; the direct
    // `arr.Count(pred)` shape is a compile error on a receiver that
    // declares `Count`.
    "      if a.Where(lambda(x) => x > threshold).Count() <> 2 then ok := 0; end; "
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
    "      var s: int := a.Aggregate(0, lambda(acc: int, x: int) => acc + x); "
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
    "      using var e: Array<int> := a.Where(lambda(x) => x mod 2 = 0).ToArray(); "
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
    "      using var e: Array<int> := a.Select(lambda(x) => x * 10).ToArray(); "
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
    "      using var e: Array<int> := a.OrderBy(lambda(x: int) => x).ToArray(); "
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
    "      for var x in a.Where(lambda(x) => x mod 2 = 0) do s := s + x; end; "
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
    "    var pred: (int) -> bool := lambda(x: int) => x > 2; "
    // fix #3342 (§26.5 rule 1 / stdlib §4.4, v0.277): composed spelling
    // `Where(pred).Count()` reaches the §5 operator pipeline; the direct
    // `arr.Count(pred)` shape is a compile error on a receiver that
    // declares `Count`.
    "    var c1: int := a.Where(pred).Count(); "
    "    var c2: int := a.Where(pred).Count(); "
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
    "        a.Zip(b.Where(lambda(y) => y > 10), lambda(x: int, y: int) => x + y).ToArray(); "
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
    "    var pred: (int) -> bool := lambda(x: int) => x > 2; "
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
    "      if a.Where(lambda(x) => x > 1).Count() <> 3 then ok := 0; end; "
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
    "      var s: int := a.Where(lambda(x) => x > 1).Select(lambda(x) => x * 10)"
    "                     .Aggregate(0, lambda(acc: int, x: int) => acc + x); "
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
    "      using var r: Array<Point> := a.OrderBy(lambda(p) => p.x)"
    "                                    .ThenBy(lambda(p) => p.y)"
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
    "    var secKey: (Point) -> int := lambda(p: Point) => p.y; "
    "    using var r1: Array<Point> := a.OrderBy(lambda(p) => p.x)"
    "                                   .ThenBy(secKey).ToArray(); "
    "    using var r2: Array<Point> := a.OrderBy(lambda(p) => p.x)"
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

// =====================================================================
// #1003 / stdlib §5 / §28: a PLAIN `var` (not `using var`) holding a
// FRESHLY-MATERIALIZED borrowing Array<T> from `.ToArray()` is trivial-
// dispose (§28 — borrowing collection, elements are borrows) and carries no
// sema disposal obligation, so it is accepted undisposed; codegen must reclaim
// its backing at scope end. Before the fix the implicit reclaim was NEVER
// emitted, so EVERY such inferred-Array local leaked its 32-byte backing — the
// leak was only masked for a SINGLE local at function top by LeakSanitizer stack
// residue, surfacing whenever two such locals (or one plus a for-in) shared the
// scope. These tests bind multiple plain-`var` ToArray locals (and the for-in
// mix) and require LSan-clean — exactly the issue's two-ToArray and ToArray-plus-
// for-in repros. The escape walk keeps a returned/aliased/captured local out of
// the reclaim set, so the `using var` and explicit-return paths are untouched.

// Two plain `var` ToArray locals in one scope (the issue's primary repro).
// Before: 64 bytes leaked in 2 allocations; after: LSan-clean. Returns 6.
const char* kPlainTwoToArray =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    var a1 := src.Where(lambda(n) => n > 0).ToArray(); "
    "    var a2 := src.Where(lambda(n) => n > 0).ToArray(); "
    "    return a1.Length + a2.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, plain_var_two_toarray_locals_leak_free_1003) {
    expectValueAndLeakFree(kPlainTwoToArray, 6);
}

// Three plain `var` ToArray locals — confirms the per-local reclaim scales (each
// freed exactly once, no double-free). Returns 9.
const char* kPlainThreeToArray =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    var a1 := src.Where(lambda(n) => n > 0).ToArray(); "
    "    var a2 := src.Where(lambda(n) => n > 0).ToArray(); "
    "    var a3 := src.Where(lambda(n) => n > 0).ToArray(); "
    "    return a1.Length + a2.Length + a3.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, plain_var_three_toarray_locals_leak_free_1003) {
    expectValueAndLeakFree(kPlainThreeToArray, 9);
}

// A plain `var` ToArray local in a scope that ALSO has a for-in loop (the issue's
// second repro — the for-in's disposal previously displaced the ToArray reclaim).
// Returns 9 (a.Length 3 + sum 1+2+3 = 6).
const char* kPlainToArrayPlusForIn =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    var a := src.Where(lambda(n) => n > 0).ToArray(); "
    "    var total := 0; "
    "    for var x in src do total := total + x; end; "
    "    return a.Length + total; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, plain_var_toarray_plus_forin_leak_free_1003) {
    expectValueAndLeakFree(kPlainToArrayPlusForIn, 9);
}

// Two plain `var` ToArray locals of Array<string> — the materialized array
// co-owns a +1 per string element (bz_array_retain_strings); the scope-end
// reclaim must release them symmetrically (string-release element disposer),
// so the test is both backing-leak-free AND element-+1-leak-free. Returns 4.
const char* kPlainTwoToArrayString =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<string>(); "
    "    src.Add(\"aa\"); src.Add(\"bbb\"); "
    "    var a := src.Where(lambda(s) => s.Length > 0).ToArray(); "
    "    var b := src.Where(lambda(s) => s.Length > 0).ToArray(); "
    "    return a.Length + b.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, plain_var_two_toarray_string_leak_free_1003) {
    expectValueAndLeakFree(kPlainTwoToArrayString, 4);
}

// SOUNDNESS: a `var a := …ToArray(); return a;` RETURNS the local — ownership
// leaves the scope, so the escape walk must EXCLUDE it from the scope-end reclaim
// (else a use-after-return / double-free in the caller). The caller binds it with
// `using var` and disposes it once. Must be LSan-clean (no leak, no UAF/DF).
// Returns 3.
const char* kReturnedToArrayLocal =
    "module M; class C "
    "  public static method MakeArr(): owned Array<int> "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    var a := src.Where(lambda(n) => n > 0).ToArray(); "
    "    return a; "
    "  end MakeArr; "
    "  public static method Main(): int "
    "    using var r := C.MakeArr(); "
    "    return r.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, returned_toarray_local_no_double_free_1003) {
    expectValueAndLeakFree(kReturnedToArrayLocal, 3);
}

// SOUNDNESS: an explicit `using var a := …ToArray()` must NOT be double-freed by
// the new plain-`var` reclaim path (it is in neither eligibility set; its single
// disposal comes from registerUsingDisposal). Two of them — LSan-clean. Ret 6.
const char* kUsingVarToArrayUntouched =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    using var a := src.Where(lambda(n) => n > 0).ToArray(); "
    "    using var b := src.Where(lambda(n) => n > 0).ToArray(); "
    "    return a.Length + b.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, using_var_toarray_not_double_freed_1003) {
    expectValueAndLeakFree(kUsingVarToArrayUntouched, 6);
}

// #1015 / §17.13 / stdlib §5 / §28: a DISCARD of a fresh-ownerless collection
// materializer (`_ := items.ToArray()` / `.Sorted()` / `.Reversed()` /
// `.ToDictionary()`) frees its freshly-allocated backing at end-of-statement.
// The §28.6/§5.11 two-axis model makes a borrowing Array/Dictionary a borrow of
// its ELEMENTS (so producerState is Borrowed and #1009's class/interface discard
// path never fires), yet the BACKING is owned by nothing — it leaked (96B / 2
// allocs) before #1015. Reclaimed via the #1004 helper (backing freed, elements
// borrowed → not disposed; the Cell is disposed once at `cell`'s using-var end).
// Cell.n == 1 (Cell still alive). Receiver retargeted from OwningArray<Cell> to a
// borrowing Array<Cell> (the #1015 backing-reclaim is receiver-agnostic) because
// #1022 now rejects copy-forms on owning collections (§4.5).
BEZEL_TEST(linq_refcount, discard_toarray_backing_freed_1015) {
    expectValueAndLeakFree(
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C public static method Main(): int "
        "  using var cell := new Cell(); "
        "  using var a := new Array<Cell>(); a.Add(cell); "
        "  _ := a.ToArray(); "
        "  return Cell.n; "
        "end Main; end C;", 1);
}

// #1015: discarding a fresh `Array<string>` ToArray releases the co-owned string
// +1 the materializer retained (no leak, no double-free of the shared chars).
BEZEL_TEST(linq_refcount, discard_toarray_string_leak_free_1015) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); a.Add(\"hi\"); a.Add(\"yo\"); "
        "  _ := a.ToArray(); "
        "  return 0; "
        "end Main; end C;", 0);
}

// #1015: discarding a fresh ToDictionary materializer frees the dict backing
// (bz_dict_free, not bz_array_free — #1013's dict-backed reclaim path). Receiver
// retargeted from OwningArray<int> to a borrowing Array<int> (the reclaim is
// receiver-agnostic) because #1022 now rejects copy-forms on owning collections.
BEZEL_TEST(linq_refcount, discard_todictionary_backing_freed_1015) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  _ := a.ToDictionary(lambda(x) => x); "
        "  return 0; "
        "end Main; end C;", 0);
}

// #1015 soundness: a discard of a NON-materializer (a trivial value) is a no-op —
// the helper's narrow guards never free a non-materializer, so no spurious free.
BEZEL_TEST(linq_refcount, discard_non_materializer_no_spurious_free_1015) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  _ := 5; return 0; "
        "end Main; end C;", 0);
}

// #1004 / stdlib §5 / §3 / §28: the copy-forms `.Sorted()` / `.Reversed()`
// shallow-copy their INPUT array into a fresh result; when that input is itself
// a fresh, ownerless materializer temporary (`X.ToArray()` / `X.Sorted()` /
// `X.Reversed()` with no `var`/`using` binding) the input is consumed only as
// the copy source and was NEVER freed → it leaked (64B: the 32B backing header
// + 32B data block, allocated by bz_lazy_to_array / bz_array_copy). The fix
// frees the input after the copy, gated strictly on the receiver expression
// being a fresh ownerless materializer CALL (isFreshOwnerlessArrayMaterializerExpr)
// — so a named-local / field / param / `move` / `new` receiver (a borrowed or
// owned-elsewhere array) is never freed, no UAF and no double-free. The four
// leak cases are LSan-clean only with BOTH the #1003 result-reclaim and this
// #1004 input-reclaim; the three soundness cases prove a non-temporary receiver
// is not double-freed.
//
// ASan (detect_leaks=1), before -> after this fix, native repro:
//   Where().ToArray().Sorted():     64B leaked / 2 allocs (bz_lazy_to_array) -> 0
//   Where().ToArray().Reversed():   64B leaked / 2 allocs -> 0
//   ToArray().Sorted().Reversed():  leaked (two intermediate temps) -> 0
//   ToArray().Sorted(comparer):     64B leaked -> 0
//   string Where().ToArray().Sorted(): backing + per-elem +1 leaked -> 0
//   named-local input (t.Sorted()): clean -> clean, no double-free (t is #1003-freed)
//   plain receiver (s.Sorted()):    clean -> clean, no UAF/double-free
//   using var = ToArray().Sorted(): clean -> clean, no double-free

// `X.Where(..).ToArray().Sorted()` — the ToArray temp is freed after Sorted's
// copy. Sorted of [3,1,2] is [1,2,3]; Length 3. Returns 3.
const char* kChainToArraySorted =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := s.Where(lambda(n) => n > 0).ToArray().Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, chain_toarray_sorted_input_leak_free_1004) {
    expectValueAndLeakFree(kChainToArraySorted, 3);
}

// `X.Where(..).ToArray().Reversed()` — the ToArray temp is freed after the
// Reversed copy. Returns 3.
const char* kChainToArrayReversed =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := s.Where(lambda(n) => n > 0).ToArray().Reversed(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, chain_toarray_reversed_input_leak_free_1004) {
    expectValueAndLeakFree(kChainToArrayReversed, 3);
}

// `X.ToArray().Sorted().Reversed()` — TWO intermediate ownerless temps (the
// ToArray result feeding Sorted, the Sorted result feeding Reversed). Each is
// freed exactly once. Sorted([3,1,2])=[1,2,3] reversed = [3,2,1]; Length 3.
// Returns 3.
const char* kChainDoubleSortedReversed =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := s.ToArray().Sorted().Reversed(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, chain_double_sorted_reversed_input_leak_free_1004) {
    expectValueAndLeakFree(kChainDoubleSortedReversed, 3);
}

// `X.ToArray().Sorted(comparer)` — the comparer copy-form also frees its fresh
// ToArray input (after the closure release). Returns 3.
const char* kChainToArraySortedComparer =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := s.ToArray().Sorted(lambda(x, y) => x - y); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, chain_toarray_sorted_comparer_input_leak_free_1004) {
    expectValueAndLeakFree(kChainToArraySortedComparer, 3);
}

// `Array<string>` chain — the ToArray temp co-owns a +1 per string element
// (bz_array_retain_strings); the #1004 input free must release them
// symmetrically (string-release element disposer), so both the 32B backing AND
// the per-element +1 are reclaimed. Returns 3.
const char* kChainToArraySortedString =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<string>(); "
    "    s.Add(\"banana\"); s.Add(\"apple\"); s.Add(\"cherry\"); "
    "    var a := s.Where(lambda(x) => x.Length > 0).ToArray().Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, chain_toarray_sorted_string_input_leak_free_1004) {
    expectValueAndLeakFree(kChainToArraySortedString, 3);
}

// SOUNDNESS: a NAMED-local input (`var t := …ToArray(); var a := t.Sorted();`).
// The receiver of `.Sorted()` is the identifier `t`, NOT a materializer CallExpr,
// so the #1004 input-free does NOT fire — `t` is owned by its own scope and is
// freed exactly once by the #1003 reclaim. Must be LSan-clean with NO
// double-free. Returns 3.
const char* kNamedLocalInputSorted =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var t := s.Where(lambda(n) => n > 0).ToArray(); "
    "    var a := t.Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, named_local_input_sorted_no_double_free_1004) {
    expectValueAndLeakFree(kNamedLocalInputSorted, 3);
}

// SOUNDNESS: a plain NAMED Array<T> receiver (`var a := s.Sorted();` where `s`
// is the using-var Array local). The receiver is the identifier `s`, not a
// materializer call → the #1004 input-free does NOT fire, so `s` (still owned by
// its `using var`) is never freed here — no UAF, no double-free. Returns 3.
const char* kPlainNamedReceiverSorted =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := s.Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, plain_named_receiver_sorted_no_double_free_1004) {
    expectValueAndLeakFree(kPlainNamedReceiverSorted, 3);
}

// SOUNDNESS: a `using var` bound to a chain (`using var a := X.ToArray().Sorted()`).
// The `.ToArray()` INPUT to Sorted is still a fresh ownerless temp → freed by
// #1004; the Sorted RESULT is disposed once via registerUsingDisposal (not the
// #1004 path). No double-free of either. Returns 3.
const char* kUsingVarChainSorted =
    "module M; class C "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    using var a := s.ToArray().Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, using_var_chain_sorted_no_double_free_1004) {
    expectValueAndLeakFree(kUsingVarChainSorted, 3);
}

// ===========================================================================
// #1006(b) / §28.3 / §28.8 / stdlib §3 / §5: dispose an UNBOUND `owned`-typed
// temporary at end-of-full-expression. Generalizes #1004's name-whitelisted
// input-free along the `owned` axis: an `owned`-RETURNING user method
// (`Fold(): owned Array<int>`) whose result is a builtin borrowing `Array<T>`,
// used UNBOUND only by a borrowing operation (a chained method receiver, a
// `.Length`/`.Count` property/method read), is reclaimed once after that use —
// it was never freed before, so it leaked (64B: backing header + data block).
// The fix gates strictly on the receiver being an `owned`-returning CALL whose
// result is a borrowing `Array<T>` (isUnboundOwnedBorrowingArrayCallExpr), so a
// BOUND (`using var`/`var`), `move`d, returned, owned-arg, or OWNING-collection
// (`OwningArray<T>`) result is never freed here — no UAF, no double-free.
//
// ASan (detect_leaks=1), before -> after, native repro:
//   P.Fold(s).Sorted():    64B leaked / 2 allocs (the owned Fold temp) -> 0
//   P.Fold(s).Reversed():  64B leaked -> 0
//   P.Fold(s).Length:      64B leaked -> 0  (property read; routes to bz_array_len)
//   owned Array<Cell> chain: backing freed once, Cell elements BORROWED (net
//                            Cell.n == 0) -> 0, no double-free of the Cells
//   using var f := P.Fold(s); f.Sorted():  clean -> clean, NO double-free
//   P.Build().Count (owned OwningArray<Cell>): NOT freed here (owning family) ->
//                            no double-free of owned Cell elements

// The confirmed #1006b repro: `P.Fold(s).Sorted()` where `Fold` returns
// `owned Array<int>`. The Fold temp is freed after Sorted's copy. Fold of
// [3,1,2] keeps the positives (all 3), Sorted has Length 3. Returns 3.
const char* kOwnedFoldSorted =
    "module M; class P "
    "  public static method Fold(src: Array<int>): owned Array<int> "
    "    => src.Where(lambda(n) => n > 0).ToArray(); "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := P.Fold(s).Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_fold_temp_sorted_chain_leak_free_1006b) {
    expectValueAndLeakFree(kOwnedFoldSorted, 3);
}

// `P.Fold(s).Reversed()` — the owned Fold temp is freed after the Reversed copy.
// Returns 3.
const char* kOwnedFoldReversed =
    "module M; class P "
    "  public static method Fold(src: Array<int>): owned Array<int> "
    "    => src.Where(lambda(n) => n > 0).ToArray(); "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var a := P.Fold(s).Reversed(); "
    "    return a.Length; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_fold_temp_reversed_chain_leak_free_1006b) {
    expectValueAndLeakFree(kOwnedFoldReversed, 3);
}

// `P.Fold(s).Length` — a `.Length` PROPERTY read is a borrow position; the owned
// Fold temp is reclaimed after the count is loaded. (Routes through the dedicated
// bz_array_len path, not the string-fallback that previously leaked it.)
// Returns 3.
const char* kOwnedFoldLength =
    "module M; class P "
    "  public static method Fold(src: Array<int>): owned Array<int> "
    "    => src.Where(lambda(n) => n > 0).ToArray(); "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var n := P.Fold(s).Length; "
    "    return n; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_fold_temp_length_property_leak_free_1006b) {
    expectValueAndLeakFree(kOwnedFoldLength, 3);
}

// `P.Fold(s).Count()` — the LINQ `.Count()` method form (an array terminal) is
// also a borrow position; the owned Fold temp is freed after the count. Returns 3.
const char* kOwnedFoldCountMethod =
    "module M; class P "
    "  public static method Fold(src: Array<int>): owned Array<int> "
    "    => src.Where(lambda(n) => n > 0).ToArray(); "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    var n := P.Fold(s).Count(); "
    "    return n; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_fold_temp_count_method_leak_free_1006b) {
    expectValueAndLeakFree(kOwnedFoldCountMethod, 3);
}

// OWNED ELEMENTS: an `owned Array<Cell>` borrowing snapshot (its elements are
// borrowed from an OwningArray<Cell> that uniquely owns them) used as a chained
// `.Reversed()` receiver. The snapshot backing is freed once; the borrowing
// element disposer is NULL (stdlib §3/§5: the caller owns the array, not its
// elements), so the Cells are NOT cascade-disposed here and are freed exactly
// once when `owners` is disposed — net Cell.n == 0, no double-free. Cell.n is
// observed AFTER both using-var scopes close (in Main, post-Run). Run returns the
// reversed snapshot Length (3); Main returns 3 + net Cell.n (0) = 3.
const char* kOwnedElemFoldReversed =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; this.V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class P "
    "  public static method Fold(src: Array<Cell>): owned Array<Cell> "
    "    => src.Where(lambda(c) => c.V > 0).ToArray(); "
    "  public static method Run(): int "
    "    using var owners := new OwningArray<Cell>(); "
    "    owners.Add(new Cell(3)); owners.Add(new Cell(1)); owners.Add(new Cell(2)); "
    "    using var view := new Array<Cell>(); "
    "    view.Add(owners.Get(0)); view.Add(owners.Get(1)); view.Add(owners.Get(2)); "
    "    using var rev := P.Fold(view).Reversed(); "
    "    return rev.Length; "
    "  end Run; "
    "  public static method Main(): int "
    "    var v := P.Run(); "
    "    return v + Cell.n; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_elem_fold_temp_reversed_dispose_once_1006b) {
    expectValueAndLeakFree(kOwnedElemFoldReversed, 3);
}

// SOUNDNESS: the owned Fold result BOUND to a `using var`, then `f.Sorted()`.
// The receiver of `.Sorted()` is the identifier `f` (NOT an owned-returning
// CallExpr), so the #1006b end-of-expression free does NOT fire — `f` is disposed
// exactly once by its using-var. Must be LSan-clean with NO double-free.
// Returns 3.
const char* kOwnedFoldBoundUsingVar =
    "module M; class P "
    "  public static method Fold(src: Array<int>): owned Array<int> "
    "    => src.Where(lambda(n) => n > 0).ToArray(); "
    "  public static method Main(): int "
    "    using var s := new Array<int>(); "
    "    s.Add(3); s.Add(1); s.Add(2); "
    "    using var f := P.Fold(s); "
    "    var a := f.Sorted(); "
    "    return a.Length; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owned_fold_bound_using_var_no_double_free_1006b) {
    expectValueAndLeakFree(kOwnedFoldBoundUsingVar, 3);
}

// SOUNDNESS: an OWNING-collection owned result (`Build(): owned OwningArray<Cell>`)
// chained as `.Count` UNBOUND. The #1006b free is gated to the BORROWING `Array`
// name-head, so an `OwningArray<T>` temp is NEVER freed here — freeing it (with an
// element cascade) would double-free the uniquely-owned Cell elements. This
// proves no double-free / UAF on the owning family (a leak of the owning temp is
// the safe, deferred direction — leak-not-dangle, §28.7). Count is 3; the Cells
// are not disposed (Cell.n stays 3), so Main returns 3 + 3 = 6.
const char* kOwningCollOwnedTempCountUnbound =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) Cell.n := Cell.n + 1; this.V := v; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class P "
    "  public static method Build(): owned OwningArray<Cell> "
    "    var r := new OwningArray<Cell>(); "
    "    r.Add(new Cell(3)); r.Add(new Cell(1)); r.Add(new Cell(2)); "
    "    return r; "
    "  end Build; "
    "  public static method Run(): int "
    "    var c := P.Build().Count; "
    "    return c; "
    "  end Run; "
    "  public static method Main(): int "
    "    var v := P.Run(); "
    "    return v + Cell.n; "
    "  end Main; "
    "end P;";

BEZEL_TEST(linq_refcount, owning_coll_owned_temp_count_unbound_no_double_free_1006b) {
    // No leak leg here (the owning temp leak is the deliberate deferred direction);
    // assert correctness AND no UAF / double-free of the owned Cell elements.
    expectValueAndNoCorruption(kOwningCollOwnedTempCountUnbound, 6);
}

// #1013 / §28.3 / stdlib §5: an UNBOUND `a.ToDictionary(...)` temporary used as
// the receiver of `.Count()` is a fresh, ownerless DICT-backed materializer — it
// must be reclaimed at end-of-statement via bz_dict_free. The #1006b unbound-temp
// reclaim was array-only (bz_array_free), so the bz_dict struct + its entries
// leaked (80 bytes). The fix extends the reclaim to dict materializers. The count
// (2) is read from the dict BEFORE the free; ASan confirms no leak.
//
// NOTE: the five ToDictionary tests below source from a borrowing `Array<int>`
// (was `OwningArray<int>`). #1022 now rejects copy-form materializers on an owning
// collection (§4.5); the dict-materializer reclaim these test is receiver-agnostic
// (value elements), so the borrowing source exercises the identical reclaim path.
const char* kToDictUnboundCount =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    do "
    "      using var a := new Array<int>(); a.Add(1); a.Add(2); "
    "      n := a.ToDictionary(lambda(x) => x).Count(); "  // unbound dict temp reclaimed
    "    end; "
    "    return n; "                                   // 2 — count correct, no leak
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, todictionary_count_unbound_temp_leak_free_1013) {
    expectValueAndLeakFree(kToDictUnboundCount, 2);
}

// #1013 SOUNDNESS: a BOUND `using var d := a.ToDictionary(...)` is freed exactly
// once by its own scope-end disposal — the `d.Count()` receiver is an identifier,
// NOT a ToDictionary CallExpr, so the unbound-temp reclaim does NOT fire (no
// double-free). LSan-clean and no double-free.
const char* kToDictBoundCount =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    do "
    "      using var a := new Array<int>(); a.Add(1); a.Add(2); "
    "      using var d := a.ToDictionary(lambda(x) => x); "
    "      n := d.Count(); "                           // bound — sole free is d's
    "    end; "
    "    return n; "                                   // 2
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, todictionary_count_bound_using_var_no_double_free_1013) {
    expectValueAndLeakFree(kToDictBoundCount, 2);
}

// #1028: a PLAIN `var d := src.ToDictionary(...)` (not `using var`) holds the sole
// reference to a fresh `owned Dictionary<K,V>`. Before the fix it was neither
// flagged for the §28.3 affine auto-drop nor reclaimed by any plain-`var` dict
// materializer path (the array side had #1003; dicts had none), so the dict struct
// + entries leaked. The fix adds a dict analog of #1003: the escape walk proves the
// local never escapes (here only `.Count` is read), so a scope-end bz_dict_free
// reclaims it exactly once. Value 3; LSan-clean (was a leak). A `using var` (above)
// stays clean — it is excluded from the eligibility set, so no double-free.
const char* kToDictPlainVarReclaimed_1028 =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    do "
    "      using var src := new Array<int>(); "
    "      src.Add(1); src.Add(2); src.Add(3); "
    "      var d := src.ToDictionary(lambda(x: int) => x); "
    "      n := d.Count; "
    "    end; "
    "    return n; "                                   // 3
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, todictionary_plain_var_reclaimed_1028) {
    expectValueAndLeakFree(kToDictPlainVarReclaimed_1028, 3);
}

// #1028 (property-discard): an UNBOUND `src.ToDictionary(...).Count` reads the
// `.Count` PROPERTY off a fresh `owned Dictionary` materializer that is then
// otherwise unreferenced. The dict-`.Count` lowering computed the scalar but —
// unlike the array-backed `.Count`/`.Length` path (#1006b) and the `.Count()`
// METHOD form (#1013) — never reclaimed the temporary, so the dict struct +
// entries leaked. The fix routes the property path through
// freeUnboundOwnedBorrowingArrayTemp (bz_dict_free), mirroring the array path.
// Value 3; LSan-clean. A bound / `using var` dict receiver is never matched
// (no double-free).
const char* kToDictPropertyDiscardReclaimed_1028 =
    "module M; class C "
    "  public static method Main(): int "
    "    using var src := new Array<int>(); "
    "    src.Add(1); src.Add(2); src.Add(3); "
    "    return src.ToDictionary(lambda(x: int) => x).Count; "
    "  end Main; "
    "end C;";

BEZEL_TEST(linq_refcount, todictionary_property_discard_reclaimed_1028) {
    expectValueAndLeakFree(kToDictPropertyDiscardReclaimed_1028, 3);
}

// #1055: a fresh `owned` collection materializer iterated DIRECTLY as a for-in
// source is reclaimed at the loop exit (reached by normal completion AND
// `break`). Before the fix the iterable backing (+ owned string elements for
// Split) leaked. The reclaim is a no-op for a bound / aliased iterable.

// string.Split() → Array<string>, iterated directly. Sum of substring lengths
// (1+2+3) = 6. The array backing + 3 strings are reclaimed at the loop exit.
BEZEL_TEST(linq_refcount, for_in_split_array_materializer_reclaimed_1055) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"a,bb,ccc\"; var total := 0; "
        "  for var p in s.Split(',') do total := total + p.Length; end; "
        "  return total; end Main; end C;", 6);
}

// Same, exited via `break` after 2 iterations — the array is still reclaimed
// once at the shared loop exit (no leak, no double-free).
BEZEL_TEST(linq_refcount, for_in_split_array_materializer_break_reclaimed_1055) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"a,bb,ccc\"; var n := 0; "
        "  for var p in s.Split(',') do n := n + 1; if n = 2 then break; end; end; "
        "  return n; end Main; end C;", 2);
}

// ToDictionary() → Dictionary, iterated directly. Sum of values (1+2+3) = 6.
// The dict struct + entries are reclaimed at the loop exit.
BEZEL_TEST(linq_refcount, for_in_todictionary_materializer_reclaimed_1055) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var src := new Array<int>(); "
        "  src.Add(1); src.Add(2); src.Add(3); var sum := 0; "
        "  for var e in src.ToDictionary(lambda(x: int) => x) do sum := sum + e.Value; end; "
        "  return sum; end Main; end C;", 6);
}

// #1057: a generic-key Dictionary for-in exited via `break` frees the
// per-iteration key boxes the increment block skipped (the boxes from the break
// index onward). Before the fix those boxes leaked (bz_dict_keys_gen). Normal
// completion frees nothing extra (no double-free).
BEZEL_TEST(linq_refcount, for_in_dict_break_frees_remaining_key_boxes_1057) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[1] := 10; d[2] := 20; d[3] := 30; var n := 0; "
        "  for var e in d do n := n + 1; if n = 2 then break; end; end; "
        "  return n; end Main; end C;", 2);
}

// #1058 / §28/§4.8: a string-keyed Dictionary for-in leaks the per-iteration
// DECODED string key objects (the loop body's malloc'd bz_string_t + dataCpy).
// Each iteration builds a fresh string object (rc := 1) that is never released.
// Fix: release in the increment block (normal completion) and at the loop-exit
// block (break path); null varSlot after each incBB release so the endBB
// release is a null-safe no-op on normal completion.

// Normal completion: three string key objects (one per dict entry) must be
// released exactly once.  Before the fix: 3 × ~32 B strObj + 3 × dataCpy =
// "Direct leak of 96 byte(s) in 3 object(s)" per LSan.  No double-free.
BEZEL_TEST(linq_refcount,
           for_in_entry_str_key_releases_decoded_str_obj_normal_1058) {
    // §4.8: for var e in Dictionary<string, int> — entry iteration.
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; d[\"c\"] := 3; var sum := 0; "
        "  for var e in d do sum := sum + e.Value; end; "
        "  return sum; end Main; end C;", 6);
}

// .Keys iteration: lowerDictKeysFor must also release the per-iteration
// strObj.  Three keys → n == 3.  Before the fix: strObj for each key leaked.
BEZEL_TEST(linq_refcount,
           for_in_keys_str_key_releases_decoded_str_obj_normal_1058) {
    // §4.8: for var k in Dictionary<string, int>.Keys
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"x\"] := 10; d[\"y\"] := 20; d[\"z\"] := 30; var n := 0; "
        "  for var k in d.Keys do n := n + 1; end; "
        "  return n; end Main; end C;", 3);
}

// Break path (entry): the in-progress iteration's strObj must be released at
// endBB, not lost.  No double-free of strObjs from completed iterations.
BEZEL_TEST(linq_refcount,
           for_in_entry_str_key_break_releases_current_str_obj_1058) {
    // Break after 2 iterations; the 3rd iteration's strObj never reaches incBB.
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"a\"] := 1; d[\"b\"] := 2; d[\"c\"] := 3; var n := 0; "
        "  for var e in d do n := n + 1; if n = 2 then break; end; end; "
        "  return n; end Main; end C;", 2);
}
