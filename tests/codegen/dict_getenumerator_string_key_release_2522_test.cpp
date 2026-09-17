// Codegen — #2522 (stdlib §4.8/§4.12; language spec §29.1, v0.236): the
// EXPLICIT `.GetEnumerator()` call on `Dictionary<string, V>` materialises
// the entries into a fresh owned `bz_array_t` of {bz_string*, V} STRUCTS
// (wrapDictionaryAsIEnumerable, #2471). Each rebuilt string key is
// ref-counted (initStringRc → rc=1). The OWNING Dispose at scope end must
// release every rebuilt key — otherwise every iteration leaks one
// ~24-byte string header + its data buffer per entry (bounded, value-
// correct, visible to LSan).
//
// Why the plain owning wrapper failed: bz_array_free's element-disposer is
// `void (*dispose_elem)(void*)` and it casts each slot to `void*` — it is
// for POINTER-element arrays only. The Dictionary materialisation stores
// {K, V} STRUCTS inline, so the plain `bz_array_free(arr, null)` never
// reaches the string at struct offset 0.
//
// Fix: a Dictionary-entry-specific owning wrapper (wrapDictionaryEntryArrayAsIEnumerable)
// whose Dispose walks arr->count slots via bz_array_ptr_at and calls
// bz_string_release on each Key (struct offset 0) before freeing the array.
//
// The test runs each program under both the JIT exit-code oracle AND the
// LeakSanitizer native-oracle (mirrors dict_record_value_leak_test.cpp). The
// exit code is the value-level correctness check; the LSan leg is the
// load-bearing check — it discriminates the leak (before #2522: 75 bytes
// leaked in 4 allocations per 3-entry iter; after #2522: 0 bytes leaked).
//
// Scope mirrors the issue: only the explicit `.GetEnumerator()` member-call
// shape on string-keyed Dictionary<string, V>. The foreach path
// (lowerDictEntryFor) already releases each rebuilt string per-iteration and
// does not leak. Non-string-keyed dictionaries (int/record/enum keys) are
// out of scope — wrapDictionaryAsIEnumerable keeps the plain
// wrapOwnedArrayAsIEnumerable for them.

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
#include "support/aot_support.hpp"
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
static const std::filesystem::path& wtDir2522() {
    return wtScratchDir("wt-2522");
}

// AOT MIGRATION (JIT->AOT): the AOT driver runs the full ownership +
// definite-assignment pipeline. Most exit codes are bounded to 8 bits
// (WEXITSTATUS), so the values here stay small (entry sums).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer native oracle (mirrors dict_record_value_leak_test.cpp's
// runUnderLeakSanitizer). Returns:
//   0  — clean (no leak, no DF, no UAF).
//   1  — sanitizer reported a problem.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir2522();
    std::string stem = unique_stem("bezel_2522_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
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
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert the JIT exit code AND, if the toolchain is present, that
// LeakSanitizer reports no leak / no double-free / no UAF.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);
}

// --- #2522 (the issue's repro) ----------------------------------------------

// Dictionary<string, int>.GetEnumerator(): three entries, iterate to the end.
// Exit code = 1 + 2 + 3 = 6. Before #2522 the rebuilt bz_string keys (alpha,
// beta, gamma) leaked: 75 bytes in 4 allocations. After #2522 the wrapper's
// Dispose walks the array and calls bz_string_release on each key → 0 leak.
const char* k2522ExplicitGetEnumerator =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"alpha\"] := 1; d[\"beta\"] := 2; d[\"gamma\"] := 3; "
    "  using var e := d.GetEnumerator(); "
    "  var n := 0; "
    "  while e.MoveNext() do n := n + e.Current.Value; end; "
    "  return n; "
    "end Main; end C;";

// --- #2522 axis-grid expansions --------------------------------------------

// Empty string-keyed dict: no entries, no rebuilt strings, the wrapper still
// walks 0 slots (count==0 short-circuits the loop), then frees the empty
// array. No leak either way; this guards against the count==0 path ICEing.
const char* k2522EmptyDict =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  using var e := d.GetEnumerator(); "
    "  var n := 0; "
    "  while e.MoveNext() do n := n + e.Current.Value; end; "
    "  return n; "
    "end Main; end C;";

// Single-entry string-keyed dict: one rebuilt string key. Exit code = 42.
// Before #2522: one ~32-byte header + 1-byte data buffer leaks. After: 0.
const char* k2522SingleEntry =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"x\"] := 42; "
    "  using var e := d.GetEnumerator(); "
    "  var n := 0; "
    "  while e.MoveNext() do n := n + e.Current.Value; end; "
    "  return n; "
    "end Main; end C;";

// Many entries (10): each iteration leaks one key before #2522. Confirms
// the fix scales linearly with entry count (no off-by-one in the loop).
// Exit code = 0+1+2+...+9 = 45.
const char* k2522ManyEntries =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"k0\"] := 0; d[\"k1\"] := 1; d[\"k2\"] := 2; d[\"k3\"] := 3; "
    "  d[\"k4\"] := 4; d[\"k5\"] := 5; d[\"k6\"] := 6; d[\"k7\"] := 7; "
    "  d[\"k8\"] := 8; d[\"k9\"] := 9; "
    "  using var e := d.GetEnumerator(); "
    "  var n := 0; "
    "  while e.MoveNext() do n := n + e.Current.Value; end; "
    "  return n; "
    "end Main; end C;";

// Multiple iterations over the SAME enumerable. Each clone borrows the
// underlying array; only the OWNING source's Dispose frees the array (and
// walks the entries). Two iterations → value reads the same 30, no leak.
// The leak was per-iteration before #2522 — this asserts that the fix is
// in Dispose (which runs once at source's scope end) and not per-MoveNext.
const char* k2522MultiIterate =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"a\"] := 10; d[\"b\"] := 20; "
    "  var n := 0; "
    "  do "
    "    using var e := d.GetEnumerator(); "
    "    while e.MoveNext() do n := n + e.Current.Value; end; "
    "  end; "
    "  do "
    "    using var e := d.GetEnumerator(); "
    "    while e.MoveNext() do n := n + e.Current.Value; end; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Record-valued string-keyed dict: same wrapDictionaryEntryArrayAsIEnumerable
// path (the entry struct is {string, R}, not {string, int}); value is a
// record stored inline (no class instance to dispose). Exit code reads a
// field. Before #2522: rebuilt string keys leak.
const char* k2522RecordValues =
    "module M; "
    "record Point(x: int, y: int); "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, Point>(); "
    "  d[\"a\"] := new Point(1, 2); "
    "  d[\"b\"] := new Point(3, 4); "
    "  using var e := d.GetEnumerator(); "
    "  var n := 0; "
    "  while e.MoveNext() do n := n + e.Current.Key.Length + e.Current.Value.x; end; "
    "  return n; "
    "end Main; end C;";

}  // namespace

// ---- #2522 (stdlib §4.8, §29.1, v0.236): string-key GetEnumerator release -

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, three_entries_no_leak) {
    // The issue's exact repro. Exit = 6. LSan leg: 0 bytes leaked (was 75).
    expectValueAndLeakFree(k2522ExplicitGetEnumerator, 6);
}

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, empty_dict_no_crash) {
    // count==0 short-circuits the dispose loop. Exit = 0.
    expectValueAndLeakFree(k2522EmptyDict, 0);
}

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, single_entry_no_leak) {
    // Single key: confirms the dispose loop runs at least once.
    expectValueAndLeakFree(k2522SingleEntry, 42);
}

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, many_entries_no_leak) {
    // 10 entries → dispose loop runs 10 times. Exit = 45.
    expectValueAndLeakFree(k2522ManyEntries, 45);
}

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, multi_iterate_no_leak) {
    // Two clones over the same owning source. Exit = 30+30 = 60. LSan leg:
    // 0 leak (the owning source's Dispose frees the array exactly once;
    // clones borrow and only free their own struct).
    expectValueAndLeakFree(k2522MultiIterate, 60);
}

BEZEL_TEST(codegen_dict_getenumerator_string_key_release_2522, record_values_no_leak) {
    // {string, Point} entries. Exit = (1+1) + (1+3) = 6. The wrapper still
    // walks the array for string release; the record value is inline so
    // no value disposal needed.
    expectValueAndLeakFree(k2522RecordValues, 6);
}