// Codegen — scope-end disposal of a dict-backed Set<T> (issue #1012).
// stdlib §4.10 `Set<T>` / language spec §28.9.
//
// A Set<T> (and OwningSet<T>) is DICT-BACKED whenever its element is generic-dict-
// eligible — a record, a non-nullable primitive/enum, or (for sets) a string;
// see genericBackingType / lowerNew. The runtime representation is a bz_dict_t*
// (the same hash table Dictionary uses), NOT a bz_array_t*.
//
// The bug (#1012): the scope-end / temp disposal of a Set routed every Set<T>
// through bz_array_free EXCEPT Set<string> (which had a dedicated bz_dict_free +
// key-release interception, added for #429). So a dict-backed Set<int> /
// Set<enum> / Set<record> was freed with bz_array_free — which reads the wrong
// header and leaks the bz_dict struct (ASan: a 64-byte struct + its entry
// allocations). Dictionary<K,V>, OwningDictionary<K,V>, and OwningArray<T> were
// all clean under the same pattern; the defect was specific to the dict-backed
// Set disposal.
//
// The fix routes ALL dict-backed Sets (genericBackingType(isSet=true) != null)
// through bz_dict_free: a string element releases its co-owned +1 first
// (dictStringKeys); a value-type/record/enum element carries no per-element
// obligation (null disposer — value types have nothing to dispose). A class /
// interface / nullable element is NOT dict-eligible (genericBackingType returns
// null) → that Set stays array-backed and keeps the bz_array_free path with its
// #888 per-element class disposer, which is correct for that representation.
//
// Verification (mirrors dict_record_value_leak_test.cpp): a LeakSanitizer native
// leg lowers the program through the full front-end (incl. ownership), links the
// ASan runtime, and runs under LSan — the load-bearing check (before the fix the
// dict struct + entries leaked). A JIT exit-code leg pins value-level behavior.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "support/unique_stem.hpp"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification (mirrors dict_record_value_leak_test.cpp). Lowers
// `source` through the full front-end (incl. ownership) to an object file, links
// against an ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no leak / no double-free / no use-after-free).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_1012_lsan");
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
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the JIT exit code (value-level behavior) and, if the toolchain is
// present, that LeakSanitizer reports no leak / no double-free / no UAF.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no DF/UAF)
}

std::string prog(const std::string& decls, const std::string& mainBody) {
    return "module M; " + decls +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// #1012 (the repro): a `using var` Set<int> — dict-backed by an int key — is
// freed via bz_dict_free at scope end, reclaiming the dict struct AND its
// entries. Before the fix bz_array_free leaked the 64-byte dict struct + entries.
BEZEL_TEST(set_dispose_leak_1012, set_int_using_var_no_leak) {
    expectValueAndLeakFree(
        prog("", "do using var s := new Set<int>(); s.Add(1); s.Add(2); "
                 "s.Add(3); end; return 0;"), 0);
}

// #1012: a Set<enum> is dict-backed (an enum lowers to an i32 value key) — same
// bz_dict_free disposal, leak-free.
BEZEL_TEST(set_dispose_leak_1012, set_enum_using_var_no_leak) {
    expectValueAndLeakFree(
        prog("enum Color Red, Green, Blue; end Color; ",
             "do using var s := new Set<Color>(); s.Add(Color.Red); "
             "s.Add(Color.Blue); end; return 0;"), 0);
}

// #1012: a Set<record> is dict-backed (the record bytes are the inline key) —
// freed via bz_dict_free; no value boxes, nothing to dispose per element.
BEZEL_TEST(set_dispose_leak_1012, set_record_using_var_no_leak) {
    expectValueAndLeakFree(
        prog("record Point(x: int, y: int); ",
             "do using var s := new Set<Point>(); s.Add(new Point(1, 2)); "
             "s.Add(new Point(3, 4)); end; return 0;"), 0);
}

// Regression (#429): a Set<string> stays leak-free — its string keys release
// their co-owned +1 (dictStringKeys) before bz_dict_free. The generalized
// interception must not change this path.
BEZEL_TEST(set_dispose_leak_1012, set_string_using_var_no_leak) {
    expectValueAndLeakFree(
        prog("", "do using var s := new Set<string>(); s.Add(\"a\"); "
                 "s.Add(\"b\"); s.Add(\"a\"); end; return 0;"), 0);
}

// Regression: an OwningSet<class> has a class element — NOT dict-eligible
// (genericBackingType returns null), so it is ARRAY-backed and keeps the
// bz_array_free path with its #888 per-element class disposer. Each element is
// disposed EXACTLY once (no leak, no double-free). The counter returns to 0.
BEZEL_TEST(set_dispose_leak_1012, owned_set_class_disposed_once) {
    expectValueAndLeakFree(
        prog("class Cell public static n: int := 0; "
             "  public constructor() Cell.n := Cell.n + 1; end constructor; "
             "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ",
             "do using var s := new OwningSet<Cell>(); s.Add(new Cell()); "
             "s.Add(new Cell()); end; return Cell.n;"), 0);
}

// Regression: a Dictionary<int,int> (the sibling dict-backed collection) is
// unaffected — still freed via bz_dict_free, leak-free.
BEZEL_TEST(set_dispose_leak_1012, dictionary_int_int_no_leak) {
    expectValueAndLeakFree(
        prog("", "do using var d := new Dictionary<int, int>(); d.Add(1, 10); "
                 "d.Add(2, 20); end; return 0;"), 0);
}
