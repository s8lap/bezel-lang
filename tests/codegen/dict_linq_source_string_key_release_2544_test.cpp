// Codegen — #2544 (stdlib §4.8/§4.12; language spec §29.1, v0.236): the
// Dictionary<string, V> LINQ SOURCE path (Calls3 `lowerToLazyLinqSource`
// Dictionary case, src/codegen/LlvmBackend_Calls3.cpp:1118-1333) materialises
// the dictionary's entries into a fresh owned `bz_array_t` of {bz_string*, V}
// STRUCTS. Each rebuilt string key is ref-counted (initStringRc -> rc=1) and
// must be released when the lazy source is dropped.
//
// Why the plain `bz_lazy_from_owned_array` failed: `bz_lazy_dispose` calls
// `bz_array_free(e->arr, nullptr)` — a null element disposer. `bz_array_free`'s
// element-disposer signature is `void(*)(void*)` and it casts each slot to
// `void*`; it is for POINTER-element arrays only. The Dictionary materialisation
// stores {K, V} STRUCTS inline, so the plain wrapper never reaches the rebuilt
// string at struct offset 0 and every iteration leaks one ~24-byte string
// header + its data buffer per entry (bounded, value-correct, visible to LSan).
//
// Sibling of #2522 (the explicit `.GetEnumerator()` path), which fixed the same
// defect shape via `wrapDictionaryEntryArrayAsIEnumerable` in
// src/codegen/LlvmBackend_Assign.cpp. The LINQ source path lives in
// src/codegen/LlvmBackend_Calls3.cpp and cannot reuse #2522's wrapper (different
// code path, different wrapper kind — bz_lazy_t, not a {vtable,arr,idx,esz}
// IEnumerator struct). The fix mirrors the runtime sibling of #1914
// (`bz_lazy_from_owned_grouping_array` → `bz_groupby_free_groups`): a new
// ctor `bz_lazy_from_owned_str_dict_array` sets `arr_str_keys := 1` on the
// bz_lazy_t, and `bz_lazy_dispose` walks the array and bz_string_release each
// Key at struct offset 0 BEFORE bz_array_free. The codegen change picks
// the new ctor when `isStringKey` is true; non-string-keyed dictionaries
// (int/record/enum keys) keep the plain wrapper — their keys are inline bytes
// with no heap ownership, so the null-disposer free is correct.
//
// The test runs each program under both the JIT exit-code oracle AND the
// LeakSanitizer native-oracle (mirrors dict_getenumerator_string_key_release_2522_test.cpp
// and dict_record_value_leak_test.cpp). The exit code is the value-level
// correctness check; the LSan leg is the load-bearing check — it discriminates
// the leak. Before the fix: 96 bytes direct + 17 bytes indirect leaked per
// 3-entry iter (3 bz_string headers + 3 data buffers). After the fix: 0.
//
// Scope mirrors the issue: only the Dictionary<string, V> as a LINQ RECEIVER
// path. The plain `for var kv in d do … end` (lowerDictEntryFor, foreach)
// already releases each rebuilt string per-iteration and does not leak.
// Non-string-keyed dictionaries (int/record/enum keys) are out of scope —
// the codegen change keeps the plain `bz_lazy_from_owned_array` for them.
// The LINQ operator shape tested here is `Where` (a deferred lazy operator)
// which is the most common receiver-position use; the fix applies identically
// to Select/Skip/Take/Count/Any/etc. — they all share the same lazy source.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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
static const std::filesystem::path& wtDir2544() {
    return wtScratchDir("wt-2544");
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

    auto dir = wtDir2544();
    std::string stem = unique_stem("bezel_2544_lsan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
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

// --- #2544 (the issue's repro) ----------------------------------------------

// Dictionary<string, int> as a LINQ receiver: three entries, Where filters all
// (Value > 0), sum the values via the iteration. Exit = 1 + 2 + 3 = 6.
// Before #2544: the lazy source's bz_lazy_dispose called bz_array_free(arr, null)
// on the {bz_string*, int} struct array — null-disposer, never reaching the
// rebuilt string at struct offset 0 — leaking 3 keys. After #2544: the new
// bz_lazy_from_owned_str_dict_array ctor sets arr_str_keys := 1, and
// bz_lazy_dispose walks the array and bz_string_release each Key before
// bz_array_free. 0 leak.
const char* k2544LinqSourceWhere =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"alpha\"] := 1; d[\"beta\"] := 2; d[\"gamma\"] := 3; "
    "  var n := 0; "
    "  for var kv in d.Where(lambda(kv) => kv.Value > 0) do "
    "    n := n + kv.Value; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// --- #2544 axis-grid expansions --------------------------------------------

// Empty string-keyed dict as a LINQ receiver: no entries, no rebuilt strings,
// the arr_str_keys dispose walk runs 0 slots (count == 0 short-circuits the
// loop), then frees the empty array. No leak either way; this guards against
// the count == 0 path ICEing or disposing a null pointer.
const char* k2544EmptyDict =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  var n := 0; "
    "  for var kv in d.Where(lambda(kv) => kv.Value > 0) do "
    "    n := n + kv.Value; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Single-entry string-keyed dict as a LINQ receiver: one rebuilt string key,
// confirms the dispose walk runs at least once. Exit = 42.
const char* k2544SingleEntry =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"x\"] := 42; "
    "  var n := 0; "
    "  for var kv in d.Where(lambda(kv) => kv.Value > 0) do "
    "    n := n + kv.Value; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Many entries (10): each iteration leaks one key before #2544. Confirms the
// fix scales linearly with entry count (no off-by-one in the dispose walk).
// Exit = 0+1+2+...+9 = 45.
const char* k2544ManyEntries =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"k0\"] := 0; d[\"k1\"] := 1; d[\"k2\"] := 2; d[\"k3\"] := 3; "
    "  d[\"k4\"] := 4; d[\"k5\"] := 5; d[\"k6\"] := 6; d[\"k7\"] := 7; "
    "  d[\"k8\"] := 8; d[\"k9\"] := 9; "
    "  var n := 0; "
    "  for var kv in d.Where(lambda(kv) => kv.Value >= 0) do "
    "    n := n + kv.Value; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Multiple iterations over the SAME LINQ receiver. Each iteration drains the
// lazy source and the source's dispose frees the array exactly once. Two
// iterations → value reads the same 6 twice = 12. The leak was per-source-
// dispose before #2544 — this asserts that the fix is in the source's dispose
// (which runs at the source's scope end), not per-iteration.
const char* k2544MultiIterate =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"a\"] := 1; d[\"b\"] := 2; "
    "  var n := 0; "
    "  do "
    "    for var kv in d.Where(lambda(kv) => kv.Value > 0) do "
    "      n := n + kv.Value; "
    "    end; "
    "  end; "
    "  do "
    "    for var kv in d.Where(lambda(kv) => kv.Value > 0) do "
    "      n := n + kv.Value; "
    "    end; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Record-valued string-keyed dict as a LINQ receiver: same {string, V} struct
// layout (V = Point, a record). Exit code reads a record field. Before #2544:
// rebuilt string keys leak (the record value is inline so no value disposal
// needed).
const char* k2544RecordValues =
    "module M; "
    "record Point(x: int, y: int); "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, Point>(); "
    "  d[\"a\"] := new Point(1, 2); "
    "  d[\"b\"] := new Point(3, 4); "
    "  var n := 0; "
    "  for var kv in d.Where(lambda(kv) => kv.Value.x > 0) do "
    "    n := n + kv.Key.Length + kv.Value.x; "
    "  end; "
    "  return n; "
    "end Main; end C;";

// Select operator (LINQ source → Select projects the entry). The lazy source
// is a `Select` enumerator built on top of the Dictionary's lazy source; the
// Dictionary source's arr_str_keys dispose runs when the source is disposed
// at the for-loop's end. Exit = 1*10 + 2*10 + 3*10 = 60.
const char* k2544SelectProject =
    "module M; "
    "class C public static method Main(): int "
    "  using var d := new Dictionary<string, int>(); "
    "  d[\"a\"] := 1; d[\"b\"] := 2; d[\"c\"] := 3; "
    "  var n := 0; "
    "  for var v in d.Where(lambda(kv) => kv.Value > 0).Select(lambda(kv) => kv.Value * 10) do "
    "    n := n + v; "
    "  end; "
    "  return n; "
    "end Main; end C;";

}  // namespace

// ---- #2544 (stdlib §4.8, §29.1, v0.236): string-key LINQ source release ----

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, three_entries_no_leak) {
    // The issue's exact repro. Exit = 6. LSan leg: 0 bytes leaked (was 113).
    expectValueAndLeakFree(k2544LinqSourceWhere, 6);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, empty_dict_no_crash) {
    // count == 0 short-circuits the dispose walk. Exit = 0.
    expectValueAndLeakFree(k2544EmptyDict, 0);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, single_entry_no_leak) {
    // Single key: confirms the dispose walk runs at least once. Exit = 42.
    expectValueAndLeakFree(k2544SingleEntry, 42);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, many_entries_no_leak) {
    // 10 entries -> dispose walk runs 10 times. Exit = 45.
    expectValueAndLeakFree(k2544ManyEntries, 45);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, multi_iterate_no_leak) {
    // Two LINQ-receiver iterations over the same dictionary. Each Where
    // materialises a fresh Dictionary snapshot (3 entries: a, b), so each
    // iteration reads 1+2 = 3 — exit = 3+3 = 6. LSan leg: 0 leak (each
    // source's dispose frees its own array exactly once through
    // arr_str_keys; no double-free because each snapshot is a fresh
    // bz_array_t).
    expectValueAndLeakFree(k2544MultiIterate, 6);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, record_values_no_leak) {
    // {string, Point} entries. Key.Length == 1 for both, Value.x == 1+3 = 4.
    // Exit = (1+1) + (1+3) = 6. The arr_str_keys dispose walk runs as for
    // {string, int} — only the Key at offset 0 needs release.
    expectValueAndLeakFree(k2544RecordValues, 6);
}

BEZEL_TEST(codegen_dict_linq_source_string_key_release_2544, select_project_no_leak) {
    // Where + Select chain. Exit = (10 + 20 + 30) = 60.
    expectValueAndLeakFree(k2544SelectProject, 60);
}
