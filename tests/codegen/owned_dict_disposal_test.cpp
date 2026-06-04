// Codegen — scope-end disposal of an OwnedDictionary<K,V>'s surviving values
// (issue #394, stdlib §4.9). Companion coverage for OwnedSet<T> (stdlib §4.10),
// whose class elements are array-backed and already disposed via #362.
//
// stdlib §4.9: "Owning hash-based map. Owns its values; keys are value-like."
// And: "There is no `DisposeElements()` on `OwnedDictionary<K, V>` — the
// dictionary's `Dispose()` cascade handles every value disposal." Keys are NOT
// owned ("`OwnedDictionary` does not support owned keys"), so only the values
// (V, constrained `V: class`) are disposed at scope end.
//
// stdlib §4.10: "`OwnedSet<T>` ... where T: class" — owns its elements; each
// surviving element is disposed once at scope end.
//
// The bug (#394): the scope-end `using var` disposal of an OwnedDictionary was
// not wired at all — neither the backing hash table nor its surviving values
// were freed. #388 wired the SETTER's disposal of a DISPLACED value
// (bz_dict_set_*_disposing); this file covers the values still present in the
// table when the `using var` scope ends, which must be disposed exactly once.
//
// Two complementary check kinds (mirroring undisposed_leak_test.cpp):
//   * destructor-count JIT tests (always run): a resource class's destructor
//     bumps a static counter, observable through the int exit code — proves the
//     scope-end disposal now runs each surviving value's destructor exactly the
//     right number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower
//     to an object file, link against an ASan-instrumented runtime, run under
//     LSan — proves the values' blocks are reclaimed with NO leak AND NO
//     double-free / use-after-free (load-bearing for the displaced+surviving
//     case, where the displaced value must NOT be disposed twice).

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
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Path to the Bezel runtime source (compiled with ASan for the LSan leg).
std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path().parent_path().parent_path();
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// LeakSanitizer verification (mirrors undisposed_leak_test.cpp). Lowers `source`
// through the full front-end to an object file, links against an
// ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_394_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();

    {
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
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the destructor-count (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak / no double-free / no use-after-free.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
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

// --- OwnedDictionary<string, R> — string-key fast path (§4.9). -------------

// Two owned values added via .Add(k, move v); both survive to scope end. The
// dictionary's Dispose() cascade disposes both values exactly once. Encoded:
// <count inside the scope, before exit> * 10 + <count after scope end>. Inside
// the scope nothing is disposed (0); at scope end both are disposed (2) → 02.
// Before #394 the values leaked and the count stayed 0 → 00.
const char* kDictStrTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwnedDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  d.Add(\"b\", move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// The displaced + surviving combination (the no-double-free case). Three keys
// are written via the indexer setter (§4.9 set(owned)); key "a" is written
// twice, so its first value is DISPLACED and disposed by the #388 setter path.
// At the displacement point R._n == 1. At scope end the THREE surviving values
// ("a"-second, "b", "c") are disposed → R._n == 4 total. The displaced value is
// already gone from the table, so it is NOT disposed again. Encoded:
// <count after displacement> * 10 + <count after scope end> = 1*10 + 4 = 14.
// Before #394: only the displaced one was disposed → 1*10 + 1 = 11.
const char* kDictStrDisplacedPlusSurvivors =
    "var afterDisplace := -1; "
    "do "
    "  using var d := new OwnedDictionary<string, R>(); "
    "  d[\"a\"] := move (new R()); "
    "  d[\"b\"] := move (new R()); "
    "  d[\"c\"] := move (new R()); "
    "  d[\"a\"] := move (new R()); "   // displaces "a"-first (disposed here)
    "  afterDisplace := R._n; "
    "end; "
    "return afterDisplace * 10 + R._n;";

// --- OwnedDictionary<int, R> — generic-key path (§4.9 / D3). ---------------

// An int key uses the generic-key runtime (bz_dict_*_gen). The owned values are
// still disposed at scope end. Two survivors → 02 (0 inside, 2 after).
const char* kDictIntTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwnedDictionary<int, R>(); "
    "  d.Add(1, move (new R())); "
    "  d.Add(2, move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// --- OwnedSet<R> — array-backed class elements (§4.10, already via #362). ---

// OwnedSet<T> where T: class is array-backed (a class element is not a generic-
// key-eligible value type), so its scope-end disposal already cascades into each
// element via the OwnedArray element-disposer (#362). This test pins that
// behavior so the dict fix does not regress it. Two elements → 02.
const char* kSetTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var s := new OwnedSet<R>(); "
    "  s.Add(move (new R())); "
    "  s.Add(move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

}  // namespace

// ---- OwnedDictionary scope-end disposal (§4.9, #394) ---------------------

BEZEL_TEST(codegen_owned_dict_394, str_key_surviving_values_disposed_4_9) {
    // Both surviving values disposed once at scope end (none disposed inside) →
    // 0*10 + 2 = 2. Before #394 the values leaked → 0. LSan leg: no leak.
    expectDisposedAndLeakFree(prog(kDictStrTwoSurvivors), 2);
}

BEZEL_TEST(codegen_owned_dict_394, str_key_displaced_plus_surviving_no_double_free_4_9) {
    // #388 setter disposes the displaced "a"-first value (count 1 at the
    // displacement point); the three survivors are disposed at scope end
    // (count 4 total). The displaced value is NOT disposed twice → 1*10 + 4 =
    // 14. Before #394 the survivors leaked → 1*10 + 1 = 11. The LSan leg is the
    // load-bearing check that there is no double-free of the displaced value.
    expectDisposedAndLeakFree(prog(kDictStrDisplacedPlusSurvivors), 14);
}

BEZEL_TEST(codegen_owned_dict_394, int_key_surviving_values_disposed_4_9) {
    // Generic-key dictionary: the surviving owned values are disposed at scope
    // end exactly like the string-key path → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kDictIntTwoSurvivors), 2);
}

// ---- OwnedSet scope-end disposal (§4.10) — regression pin ----------------

BEZEL_TEST(codegen_owned_set_394, class_elements_surviving_disposed_4_10) {
    // OwnedSet<R> (R a class) is array-backed; its surviving elements are
    // disposed at scope end via the OwnedArray element-disposer (#362). This
    // must remain true after the dict fix → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kSetTwoSurvivors), 2);
}
