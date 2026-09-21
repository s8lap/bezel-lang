// Codegen — Dictionary<K,V>.Clear() and OwningDictionary<K,V>.Clear()
// (stdlib §4.8 / §4.9).
//
// stdlib §4.8 (`Dictionary<K, V>`): "public method Clear();". The borrowing
// dictionary owns nothing; Clear empties the table (and frees each key's byte
// copy) but disposes no value — values are borrows the caller still owns. For a
// RECORD value type the borrowing dictionary heap-BOXES each value (the value is
// an aggregate too wide for the inttoptr slot); Clear must reclaim each surviving
// box (value storage, nothing to dispose inside — a record is a value type).
//
// stdlib §4.9 (`OwningDictionary<K, V> where V: class`): "public method Clear();
// // disposes every value". The owning dictionary owns its values; Clear
// disposes every surviving value EXACTLY ONCE. The table stays allocated and
// usable afterward (Count is 0), so a value re-added after Clear is disposed once
// at scope end — and a value disposed by Clear is NOT re-disposed at scope end
// (no double-free).
//
// Before this change Dictionary.Clear() was unhandled in codegen (it returned
// nullptr, which crashed at the statement site), so these are load-bearing.
//
// Two complementary check kinds (mirroring owned_dict_disposal_test.cpp):
//   * destructor-count JIT tests (always run): a resource class R's destructor
//     bumps a static counter, observable through the int exit code — proves
//     Clear disposes each surviving value the right number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower to
//     an object file, link against an ASan-instrumented runtime, run under LSan
//     — proves the values' / boxes' blocks are reclaimed with NO leak AND NO
//     double-free / use-after-free (load-bearing for the Clear-then-scope-end
//     and Clear-then-readd interactions).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirDclr() {
    return wtScratchDir("wt-dclr");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}


// LeakSanitizer verification (mirrors owned_dict_disposal_test.cpp). Lowers
// `source` through the full front-end to an object file, links against an
// ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDirDclr();
    std::string stem = unique_stem("bezel_dclr_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
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
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

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

// Assert the exit code (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak / no double-free / no use-after-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// A resource class R (destructor bumps R._n) and a Main holding the given body.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Borrowing Dictionary<K,V>.Clear() (§4.8) — value semantics, no disposal.
// ---------------------------------------------------------------------------

// §4.8: Clear empties the table; Count is 0 afterward. A borrowing dictionary
// owns nothing, so no destructor runs. before*10 + after = 2*10 + 0 = 20.
BEZEL_TEST(dict_clear_4_8, str_key_int_value_clears_to_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var before := d.Count; "
        "  d.Clear(); "
        "  return before * 10 + d.Count; "
        "end Main; end C;"), 20);
}

// §4.8 / D3: an int key uses the generic-key table (bz_dict_*_gen); Clear must
// empty it too. before*10 + after = 3*10 + 0 = 30.
BEZEL_TEST(dict_clear_4_8, generic_key_clears_to_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(1, 10); d.Add(2, 20); d.Add(3, 30); "
        "  var before := d.Count; "
        "  d.Clear(); "
        "  return before * 10 + d.Count; "
        "end Main; end C;"), 30);
}

// §4.8: the table stays allocated and usable after Clear — entries added after
// Clear are present and found. int(has "x")*100 + Count = 1*100 + 2 = 102.
BEZEL_TEST(dict_clear_4_8, table_reusable_after_clear) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); "
        "  d.Clear(); "
        "  d.Add(\"x\", 9); d.Add(\"y\", 8); "
        "  return int(d.ContainsKey(\"x\")) * 100 + d.Count; "
        "end Main; end C;"), 102);
}

// §4.8: Clear on an already-empty dictionary is a no-op (Count stays 0).
BEZEL_TEST(dict_clear_4_8, clear_on_empty_is_noop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Clear(); "
        "  return d.Count; "
        "end Main; end C;"), 0);
}

// §4.8: a RECORD value is heap-BOXED; Clear must reclaim each surviving box
// (value storage; nothing to dispose inside). The LSan leg is load-bearing —
// it proves the boxes are freed with no leak and no double-free. before*10 +
// after = 2*10 + 0 = 20.
BEZEL_TEST(dict_clear_4_8, record_value_boxes_reclaimed_on_clear) {
    expectValueAndLeakFree(
        "module M; record P(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, P>(); "
        "  d.Add(\"a\", new P(1, 2)); d.Add(\"b\", new P(3, 4)); "
        "  var before := d.Count; "
        "  d.Clear(); "
        "  return before * 10 + d.Count; "
        "end Main; end C;", 20);
}

// ---------------------------------------------------------------------------
// OwningDictionary<K,V>.Clear() (§4.9) — disposes every value exactly once.
// ---------------------------------------------------------------------------

// §4.9: "Clear(); // disposes every value". Three owned values; Clear disposes
// all three EXACTLY ONCE. afterClear reads R._n right after Clear (== 3). The
// table is now empty, so the scope-end Dispose disposes 0 more → final R._n is
// still 3 (NO double-free). afterClear*10 + final = 3*10 + 3 = 33.
BEZEL_TEST(owned_dict_clear_4_9, str_key_clear_disposes_every_value_once) {
    expectValueAndLeakFree(prog(
        "var afterClear := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  d.Add(\"c\", move (new R())); "
        "  d.Clear(); "
        "  afterClear := R._n; "
        "end; "
        "return afterClear * 10 + R._n;"), 33);
}

// §4.9 / D3: an int key uses the generic-key table; Clear still disposes every
// owned value once. Two values → afterClear 2; scope-end disposes 0 more → 2.
// afterClear*10 + final = 2*10 + 2 = 22.
BEZEL_TEST(owned_dict_clear_4_9, generic_key_clear_disposes_every_value_once) {
    expectValueAndLeakFree(prog(
        "var afterClear := -1; "
        "do "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, move (new R())); "
        "  d.Add(2, move (new R())); "
        "  d.Clear(); "
        "  afterClear := R._n; "
        "end; "
        "return afterClear * 10 + R._n;"), 22);
}

// §4.9: the dictionary is reusable after Clear and still OWNS values added
// afterward. Clear disposes the 2 original values (afterClear == 2); one new
// value is added; the scope-end Dispose disposes that 1 new value once → final
// R._n is 3. afterClear*10 + final = 2*10 + 3 = 23. This is the no-double-free /
// no-leak interaction between Clear and scope-end disposal (LSan leg checks it).
BEZEL_TEST(owned_dict_clear_4_9, reusable_after_clear_still_owns_values) {
    expectValueAndLeakFree(prog(
        "var afterClear := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  d.Clear(); "
        "  afterClear := R._n; "
        "  d.Add(\"x\", move (new R())); "
        "end; "
        "return afterClear * 10 + R._n;"), 23);
}

// §4.9: Clear on an empty OwningDictionary is a no-op — no destructor runs, the
// scope-end Dispose finds an empty table → R._n stays 0.
BEZEL_TEST(owned_dict_clear_4_9, clear_on_empty_disposes_nothing) {
    expectValueAndLeakFree(prog(
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Clear(); "
        "end; "
        "return R._n;"), 0);
}
