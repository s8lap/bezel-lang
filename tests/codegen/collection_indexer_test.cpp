// Codegen — heap-collection indexer `a[i]` get / `a[i] := v` set (issue #388,
// stdlib §4.4 / §4.5 / §4.8 / §4.9).
//
// Before this fix, sema only typed `IndexExpr` element access for inline arrays
// `[N]T` and strings; the built-in HEAP collections (Array<T>, OwningArray<T>,
// Dictionary<K,V>, OwningDictionary<K,V>) were NOT typed for `a[i]`. As a
// result codegen had no element type to lower against:
//   * `a[i]` (read) fell to the inline-array GEP path and returned garbage.
//   * `a[i] := v` (setter) was silently dropped before reaching codegen.
//
// The indexers, per the stdlib:
//   * Array<T>            (§4.4): `public property [i: int]: T;`
//                          read returns the slot; setter stores a borrow, NO
//                          disposal of the previous slot (the array borrows).
//   * OwningArray<T>       (§4.5): `[i: int]: T { get; set(owned); }`
//                          getter borrows; setter takes `owned` and DISPOSES
//                          the previous slot value before storing the new one.
//   * Dictionary<K,V>     (§4.8): `public property [key: K]: V;`
//                          read returns the value (`dict[key]` raises
//                          KeyNotFoundException if absent, §4.8); setter stores
//                          a borrow, no disposal.
//   * OwningDictionary<K,V>(§4.9): `[key: K]: V { get; set(owned); }`
//                          getter borrows; setter takes `owned` and disposes
//                          the previous value before storing the new one.
//
// Two complementary check kinds (mirroring undisposed_leak_test.cpp /
// owned_collection_ops_test.cpp):
//   * destructor-count JIT tests (always run): the element's destructor bumps a
//     static counter, observable via the int exit code — proves the setter
//     disposes the displaced element exactly once.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower
//     the program to an object file, link against an ASan-instrumented runtime,
//     run under LSan — proves no leak / no double-free / no use-after-free from
//     the new disposing-set free path.

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
using bezel::test::runAotOut;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir388() {
    return wtScratchDir("wt-388");
}

// AOT MIGRATION (JIT->AOT): the AOT driver (runAot) runs the full ownership +
// definite-assignment pipeline — the stricter oracle. Two tests whose packed
// value exceeds 255 print it and assert on stdout via runAotOut.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}


// LeakSanitizer verification (modeled on undisposed_leak_test.cpp). Returns:
//   0  — clean: no leak, no use-after-free, no double-free.
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir388();
    std::string stem = unique_stem("bezel_388_lsan");
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the destructor-count (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

}  // namespace

// ---------------------------------------------------------------------------
// Array<T> — §4.4 indexer get/set (value elements). Borrowing array: the
// setter stores a value into the slot, no disposal.
// ---------------------------------------------------------------------------

// §4.4: `a[i]` read returns the stored element. `+ 0` defeats the borrow-escape
// guard so the bare read value reaches the return (load-bearing: before the fix
// the read fell to the inline-array path and returned GARBAGE, not 20).
BEZEL_TEST(collection_indexer_388, array_int_indexer_get_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var x := a[1] + 0; "
        "  return x; "                                  // 20
        "end Main; end C;"), 20);
}

// §4.4: `a[i] := v` stores into the slot; a later `a.Get(i)` reads it back.
// Load-bearing: before the fix the setter was silently dropped and Get returned
// the original 20.
BEZEL_TEST(collection_indexer_388, array_int_indexer_set_4_4) {
    // Packed value 31099 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  a[1] := 99; "
        "  Console.WriteLine(a.Get(0) * 100 + a.Get(1) * 1 + a.Get(2) * 1000); return 0; " // 10*100+99+30000=31099
        "end Main; end C;"), "31099");
}

// §4.4: set then read through the indexer itself (round-trip), no method calls.
BEZEL_TEST(collection_indexer_388, array_int_indexer_set_then_get_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  a[0] := 7; a[1] := 8; "
        "  var s := a[0] + a[1]; "
        "  return s; "                                  // 7 + 8 = 15
        "end Main; end C;"), 15);
}

// §4.4 / §3: a string-element Array<string> indexer round-trips a pointer slot.
// .Length on the read value proves the right string reached the slot.
BEZEL_TEST(collection_indexer_388, array_string_indexer_set_get_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"x\"); a.Add(\"yy\"); "
        "  a[0] := \"zzzzz\"; "
        "  var n := a[0].Length + a[1].Length; "        // 5 + 2 = 7
        "  return n; "
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// OwningArray<T> — §4.5 indexer get/set. Setter takes `owned` and DISPOSES the
// previous slot value (the #381 blocker). This is the load-bearing case.
// ---------------------------------------------------------------------------

// §4.5: "the indexer setter takes ownership and disposes the previous slot
// value." `a[0] := move (new R())` disposes the element previously at slot 0
// exactly once. Encoded: <count right after the set> * 10 + <count after the
// array's own scope-end Dispose>. The set disposes 1 (the displaced element);
// scope-end disposes the other 2 (the new slot-0 element + slot-1) → 13.
// Before the fix the setter was a no-op → the moved-in element was discharged
// elsewhere and the displaced one survived to scope-end (count differs).
BEZEL_TEST(collection_indexer_388, owned_array_indexer_set_disposes_previous_4_5) {
    const char* src =
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterSet := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R())); "
        "    a.Add(move (new R())); "
        "    a[0] := move (new R()); "
        "    afterSet := R._n; "
        "  end; "
        "  return afterSet * 10 + R._n; "               // 1*10 + 3 = 13
        "end Main; end C;";
    expectDisposedAndLeakFree(src, 13);
}

// §4.5: the setter installs the new element so a later read sees it (the new
// element is alive; the displaced one is gone). a.Count is unchanged by a set.
// We read back through .Count (a borrow of the slot would escape; Count does
// not), proving the slot still holds exactly one live element after replacement.
BEZEL_TEST(collection_indexer_388, owned_array_indexer_set_keeps_count_4_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); a.Add(move (new R())); "
        "  a[1] := move (new R()); "
        "  return a.Count * 10 + R._n; "                // count 2, one disposed → 21
        "end Main; end C;"), 21);
}

// §4.5: setting EVERY slot disposes each previous element exactly once; the
// array's scope-end Dispose then disposes the new occupants. Two slots, each
// replaced once → 2 disposed at the sets, 2 disposed at scope end → total 4.
BEZEL_TEST(collection_indexer_388, owned_array_indexer_set_all_slots_4_5) {
    const char* src =
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterSets := -1; "
        "  do "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(move (new R())); a.Add(move (new R())); "
        "    a[0] := move (new R()); "
        "    a[1] := move (new R()); "
        "    afterSets := R._n; "
        "  end; "
        "  return afterSets * 10 + R._n; "              // 2*10 + 4 = 24
        "end Main; end C;";
    expectDisposedAndLeakFree(src, 24);
}

// ---------------------------------------------------------------------------
// Dictionary<K,V> — §4.8 indexer get/set (borrowing; no disposal on set).
// ---------------------------------------------------------------------------

// §4.8: `dict[key]` reads the stored value (string key). Load-bearing: before
// the fix the read returned garbage (0).
BEZEL_TEST(collection_indexer_388, dict_str_int_indexer_get_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 5); d.Add(\"b\", 7); "
        "  var x := d[\"b\"] + 0; "
        "  return x; "                                  // 7
        "end Main; end C;"), 7);
}

// §4.8: `dict[key] := v` stores (insert-or-replace). A read of the same key
// then returns the stored value. Load-bearing: before the fix the setter was
// silently dropped.
BEZEL_TEST(collection_indexer_388, dict_str_int_indexer_set_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 5); "
        "  d[\"a\"] := 11; "                            // replace existing
        "  d[\"c\"] := 22; "                            // insert new
        "  var s := d[\"a\"] + d[\"c\"]; "
        "  return s; "                                  // 11 + 22 = 33
        "end Main; end C;"), 33);
}

// §4.8: an int-keyed dictionary routes through the generic-key runtime; the
// indexer get/set must follow the same path.
BEZEL_TEST(collection_indexer_388, dict_int_int_indexer_set_get_4_8) {
    // 311 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d[1] := 100; d[2] := 200; "
        "  d[1] := 111; "                               // replace
        "  var s := d[1] + d[2]; "
        "  Console.WriteLine(s); return 0; "            // 111 + 200 = 311
        "end Main; end C;"), "311");
}

// §4.8: `dict[key]` raises KeyNotFoundException when the key is absent. The
// throw is caught and a sentinel returned, proving the guard fires (load-
// bearing: before the fix the read returned garbage rather than throwing).
BEZEL_TEST(collection_indexer_388, dict_indexer_missing_key_throws_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 5); "
        "  try "
        "    var x := d[\"missing\"] + 0; "
        "    return x; "                                // unreachable
        "  catch KeyNotFoundException => return 42; "
        "  end; "
        "  return -1; "
        "end Main; end C;"), 42);
}

// ---------------------------------------------------------------------------
// OwningDictionary<K,V> — §4.9 indexer get/set. Setter takes `owned` and
// disposes the previous value (analogous to OwningArray, §4.5).
//
// NOTE: these tests observe the indexer setter's own disposal of the DISPLACED
// value, read BEFORE the dictionary's scope-end Dispose. The OwningDictionary
// scope-end value-disposal path (bz_dict_free is currently called with a null
// dispose_val — see LlvmBackend_Calls.cpp / LlvmBackend_Setup.cpp) is a
// PRE-EXISTING, separately-tracked gap (marked "Dict/Set disposal is not wired
// in this slice"), out of scope for #388 (the indexer). So these tests do not
// assert on counts that depend on scope-end value disposal; no LSan leg, since
// the unwired scope-end path would (independently of #388) leak the surviving
// value.
// ---------------------------------------------------------------------------

// §4.9: "indexer set(owned); owned-taking; disposes previous." Setting an
// existing key disposes the value previously stored under it EXACTLY once. The
// count is read right after the replacing set (before scope end): the displaced
// value is disposed (1), the new value is alive (not disposed). Load-bearing:
// before the fix the setter was a no-op and nothing was disposed (0).
BEZEL_TEST(collection_indexer_388, owned_dict_indexer_set_disposes_previous_4_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"k\", move (new R())); "
        "  d[\"k\"] := move (new R()); "                // replace → dispose old
        "  return R._n; "                               // 1 (displaced disposed once)
        "end Main; end C;"), 1);
}

// §4.9 (generic-key): the same disposing-set semantics over an int key, which
// routes through the generic-key runtime (bz_dict_set_gen_disposing).
BEZEL_TEST(collection_indexer_388, owned_dict_int_key_set_disposes_previous_4_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(7, move (new R())); "
        "  d[7] := move (new R()); "                    // replace → dispose old
        "  return R._n; "                               // 1
        "end Main; end C;"), 1);
}

// §4.9: inserting a NEW key via the indexer setter disposes nothing (no previous
// value under the key). The count read right after the insert is 0; Count is 1.
// Load-bearing: before the fix the setter was a no-op (Count would be 0).
BEZEL_TEST(collection_indexer_388, owned_dict_indexer_insert_no_dispose_4_9) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[\"k\"] := move (new R()); "                // fresh key → no disposal
        "  return R._n * 10 + d.Count; "                // 0*10 + 1 = 1
        "end Main; end C;"), 1);
}

// §4.4 / §31.2: the Array<T> indexer read bounds-checks the index and raises
// IndexOutOfRangeException when out of range. The throw is caught (a non-const
// out-of-range index, so no compile-time elision); the body's `r := 9` is NOT
// reached. Load-bearing: before the fix the read fell to the inline-array path
// (no heap bounds check) and read out-of-bounds heap memory silently.
BEZEL_TEST(collection_indexer_388, array_indexer_out_of_range_throws_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  var i := 5; var r := 5; "
        "  try var x := a[i] + 0; r := 9; "
        "  catch IndexOutOfRangeException => r := 7; end; "
        "  return r; "                                  // catch fires → 7
        "end Main; end C;"), 7);
}

// §4.4 / §31.2: the Array<T> indexer SETTER also bounds-checks. An out-of-range
// `a[i] := v` raises IndexOutOfRangeException (caught here).
BEZEL_TEST(collection_indexer_388, array_indexer_set_out_of_range_throws_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); "
        "  var i := 9; var r := 5; "
        "  try a[i] := 3; r := 9; "
        "  catch IndexOutOfRangeException => r := 7; end; "
        "  return r; "                                  // catch fires → 7
        "end Main; end C;"), 7);
}
