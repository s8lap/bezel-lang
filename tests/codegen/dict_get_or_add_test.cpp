// Codegen — Dictionary<K,V>.GetOrAdd(key, factory) and
// OwningDictionary<K,V>.GetOrAdd(key, factory) (stdlib §4.8 / §4.9).
//
// stdlib §4.8 (`Dictionary<K, V>`):
//   public method GetOrAdd(key: K, factory: (K) -> V): V;
// If `key` is present, GetOrAdd returns the existing value and does NOT invoke
// the factory. Otherwise it invokes `factory(key)` to produce a V, stores it
// under `key` (the same insert path Add/Set uses — record values heap-boxed),
// and returns the now-stored value. The borrowing dictionary owns nothing; the
// returned V is a borrow/value the caller already manages.
//
// stdlib §4.9 (`OwningDictionary<K, V> where V: class`):
//   public method GetOrAdd(key: K, factory: (K) -> owned V): V;  // returns borrow
// "the factory produces an `owned V`; the dictionary stores the value (taking
// ownership) and the call returns a borrow of the now-stored value." (§4.9).
// On the MISS path: invoke factory → store the +1 owned V into the dict (the
// dict now owns it) → return a BORROW of the stored slot (NOT a transfer; per
// language spec §28.3 the dict still owns it). The stored value is disposed
// EXACTLY ONCE — by the dict at scope-end (§4.9 Dispose cascade), never by the
// caller (the return was a borrow). On the HIT path: the factory is NOT invoked
// (no owned V is produced, so nothing leaks), and the existing slot's borrow is
// returned.
//
//   * LEAK if: the factory runs on the hit path (an owned V is produced and
//     never stored/disposed), or it runs on the miss path but is not stored.
//   * DOUBLE-FREE if: the factory result is stored AND the return is treated as
//     an owned transfer (dict disposes at scope-end, caller disposes too).
//
// Two complementary check kinds (mirroring dict_clear_test.cpp):
//   * destructor-count / side-effect JIT tests (always run): observable through
//     the int exit code — prove the factory runs on the miss path and NOT on the
//     hit path, and (for owned values) that each value is disposed the right
//     number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower to
//     an object file, link against an ASan-instrumented runtime, run under LSan
//     — prove the owned value is reclaimed with NO leak (miss path stored it) and
//     NO double-free (the return was a borrow), and that a hit-path call leaks
//     nothing (the factory never ran).

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
static const std::filesystem::path& wtDirGoa() {
    return wtScratchDir("wt-goa");
}

// AOT MIGRATION (JIT->AOT): the AOT driver (runAot) runs the full ownership +
// definite-assignment pipeline — the stricter oracle. Most packed values here
// exceed the 8-bit exit-code ceiling, so those tests print the value and assert
// on stdout via runAotOut (and the *OutAndLeakFree helper keeps the LSan leg).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}


// LeakSanitizer verification (mirrors dict_clear_test.cpp). Lowers `source`
// through the full front-end to an object file, links against an
// ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDirGoa();
    std::string stem = unique_stem("bezel_goa_lsan");
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

// Assert an observable value that exceeds the 8-bit exit-code ceiling: the
// program prints its result via Console.WriteLine and returns 0, so the value is
// checked on captured stdout (runAotOut) while the LSan leg still runs the same
// source for leak / double-free / use-after-free coverage.
void expectValueOutAndLeakFree(const std::string& source,
                               const std::string& expectedStdout) {
    BEZEL_EXPECT_EQ(runAotOut(source), expectedStdout);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// A resource class R (destructor bumps R._n; constructor takes a tag readable
// via Tag) and a Main holding the given body. F._n counts factory invocations.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class F public static _n: int := 0; end F; "
        "class R "
        "  public static _n: int := 0; "
        "  public _tag: int; "
        "  public constructor(t: int) _tag := t; end constructor; "
        "  public method Tag(): int return _tag; end Tag; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Borrowing Dictionary<K,V>.GetOrAdd (§4.8) — value semantics, no disposal.
// ---------------------------------------------------------------------------

// §4.8 MISS: empty dict, key absent → the factory is invoked, the produced V is
// stored under the key, and that value is returned. F._n == 1 (factory ran).
// Encoded: returned*100 + F._n*10 + Count = 7*100 + 1*10 + 1 = 711.
BEZEL_TEST(dict_get_or_add_4_8, str_key_int_value_miss_invokes_factory_and_stores) {
    // 711 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class F public static _n: int := 0; end F; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => do F._n := F._n + 1; return 7; end); "
        "  Console.WriteLine(got * 100 + F._n * 10 + d.Count); return 0; "
        "end Main; end C;"), "711");
}

// §4.8 HIT: the key is already present → GetOrAdd returns the EXISTING value and
// does NOT invoke the factory (F._n stays 0). The factory would return 99, a
// distinguishable value, proving it never ran.
// Encoded: returned*100 + F._n*10 + Count = 5*100 + 0*10 + 1 = 501.
BEZEL_TEST(dict_get_or_add_4_8, str_key_int_value_hit_returns_existing_no_factory) {
    // 501 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class F public static _n: int := 0; end F; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 5); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => do F._n := F._n + 1; return 99; end); "
        "  Console.WriteLine(got * 100 + F._n * 10 + d.Count); return 0; "
        "end Main; end C;"), "501");
}

// §4.8 MISS then read-back: after a miss stores the factory value, a subsequent
// indexer read returns the SAME value (the store is durable). got == d["a"] == 42
// → got*10 + d["a"] = 42*10 + 42 = 462.
BEZEL_TEST(dict_get_or_add_4_8, str_key_miss_stored_value_is_readable_afterward) {
    // 462 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => 42); "
        "  Console.WriteLine(got * 10 + d[\"a\"]); return 0; "
        "end Main; end C;"), "462");
}

// §4.8 / D3 generic (int) key — MISS routes through the generic-key table
// (bz_dict_*_gen). Factory runs, value stored, returned. F._n == 1.
// Encoded: returned*100 + F._n*10 + Count = 8*100 + 1*10 + 1 = 811.
BEZEL_TEST(dict_get_or_add_4_8, generic_key_miss_invokes_factory_and_stores) {
    // 811 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class F public static _n: int := 0; end F; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  var got := d.GetOrAdd(3, lambda(k: int) => do F._n := F._n + 1; return 8; end); "
        "  Console.WriteLine(got * 100 + F._n * 10 + d.Count); return 0; "
        "end Main; end C;"), "811");
}

// §4.8 / D3 generic (int) key — HIT returns the existing value, factory not run.
// Encoded: returned*100 + F._n*10 + Count = 6*100 + 0*10 + 1 = 601.
BEZEL_TEST(dict_get_or_add_4_8, generic_key_hit_returns_existing_no_factory) {
    // 601 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class F public static _n: int := 0; end F; "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Add(3, 6); "
        "  var got := d.GetOrAdd(3, lambda(k: int) => do F._n := F._n + 1; return 77; end); "
        "  Console.WriteLine(got * 100 + F._n * 10 + d.Count); return 0; "
        "end Main; end C;"), "601");
}

// §4.8 string value — MISS stores a string; a subsequent read returns the same
// string (length observed). factory returns "hello" → got.Length*10 + d["a"].Length
// = 5*10 + 5 = 55.
BEZEL_TEST(dict_get_or_add_4_8, str_value_miss_stores_and_reads_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, string>(); "
        "  var got := d.GetOrAdd(\"k\", lambda(k: string) => \"hello\"); "
        "  return got.Length * 10 + d[\"k\"].Length; "
        "end Main; end C;"), 55);
}

// §4.8 RECORD value — a record V is an aggregate too wide for the inttoptr value
// slot, so the borrowing dictionary HEAP-BOXES it on store (the same insert path
// Add/Set uses); GetOrAdd's miss path boxes the factory result and the return /
// a subsequent indexer read load the boxed record back. The LSan leg is
// load-bearing: it proves the box is freed at scope end with no leak (and the
// closure cell is reclaimed too) and no double-free. got == again == P(3,4):
// got.X*1000 + got.Y*100 + again.X*10 + again.Y = 3*1000 + 4*100 + 3*10 + 4 = 3434.
BEZEL_TEST(dict_get_or_add_4_8, record_value_miss_boxes_and_reads_back) {
    // 3434 > 255 → print + runAotOut (LSan leg preserved).
    expectValueOutAndLeakFree(
        "module M; record P(X: int, Y: int); "
        "class C public static method Main(): int "
        "  using var d := new Dictionary<string, P>(); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => new P(3, 4)); "
        "  var again := d[\"a\"]; "
        "  Console.WriteLine(got.X * 1000 + got.Y * 100 + again.X * 10 + again.Y); return 0; "
        "end Main; end C;", "3434");
}

// ---------------------------------------------------------------------------
// OwningDictionary<K,V>.GetOrAdd (§4.9) — owned→borrow handoff.
// ---------------------------------------------------------------------------

// §4.9 MISS: empty dict → the factory produces an owned R, the dict STORES it
// (taking ownership), and GetOrAdd returns a BORROW of the stored value. Inside
// the scope nothing is disposed (the dict owns the value). At scope end the dict
// disposes its one value EXACTLY ONCE (no leak: it was stored; no double-free:
// the return was a borrow, not a transfer). The borrow is usable: got.Tag() == 7.
// Encoded: tag*100 + inside*10 + final = 7*100 + 0*10 + 1 = 701.
BEZEL_TEST(owned_dict_get_or_add_4_9, str_key_miss_stores_owned_and_returns_borrow) {
    // 701 > 255 → print + runAotOut (LSan leg preserved).
    expectValueOutAndLeakFree(prog(
        "var tag := -1; var inside := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => move (new R(7))); "
        "  tag := got.Tag(); "
        "  inside := R._n; "
        "end; "
        "Console.WriteLine(tag * 100 + inside * 10 + R._n); return 0;"), "701");
}

// §4.9 HIT: the key is already present → the factory is NOT invoked (no owned R
// is produced, so nothing leaks), and GetOrAdd returns a borrow of the EXISTING
// value (tag 3, not the factory's 9). F._n stays 0 (factory never ran). The dict
// owns the single pre-existing value, disposed once at scope end.
// Encoded: tag*100 + F._n*10 + final = 3*100 + 0*10 + 1 = 301.
BEZEL_TEST(owned_dict_get_or_add_4_9, str_key_hit_no_factory_returns_existing_borrow) {
    // 301 > 255 → print + runAotOut (LSan leg preserved).
    expectValueOutAndLeakFree(prog(
        "var tag := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R(3))); "
        "  var got := d.GetOrAdd(\"a\", lambda(k: string) => do F._n := F._n + 1; return move (new R(9)); end); "
        "  tag := got.Tag(); "
        "end; "
        "Console.WriteLine(tag * 100 + F._n * 10 + R._n); return 0;"), "301");
}

// §4.9 / D3 generic (int) key — MISS stores the owned R via the generic-key
// table; the return is a borrow; the dict disposes the value once at scope end.
// got.Tag() == 5 → tag*100 + inside*10 + final = 5*100 + 0*10 + 1 = 501.
BEZEL_TEST(owned_dict_get_or_add_4_9, generic_key_miss_stores_owned_and_returns_borrow) {
    // 501 > 255 → print + runAotOut (LSan leg preserved).
    expectValueOutAndLeakFree(prog(
        "var tag := -1; var inside := -1; "
        "do "
        "  using var d := new OwningDictionary<int, R>(); "
        "  var got := d.GetOrAdd(2, lambda(k: int) => move (new R(5))); "
        "  tag := got.Tag(); "
        "  inside := R._n; "
        "end; "
        "Console.WriteLine(tag * 100 + inside * 10 + R._n); return 0;"), "501");
}

// §4.9 MISS then HIT on the SAME key — the second GetOrAdd must NOT invoke its
// factory (key now present from the first miss) and must return the borrow of the
// first value. Only ONE owned value ever exists, disposed once at scope end (no
// leak, no double-free). The second factory's R(9) is never produced (F._n == 0).
// Encoded: tag1*100 + tag2*10 + final = 4*100 + 4*10 + 1 = 441.
BEZEL_TEST(owned_dict_get_or_add_4_9, second_call_same_key_is_hit_no_second_value) {
    // 441 > 255 → print + runAotOut (LSan leg preserved).
    expectValueOutAndLeakFree(prog(
        "var tag1 := -1; var tag2 := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var g1 := d.GetOrAdd(\"a\", lambda(k: string) => move (new R(4))); "
        "  tag1 := g1.Tag(); "
        "  var g2 := d.GetOrAdd(\"a\", lambda(k: string) => do F._n := F._n + 1; return move (new R(9)); end); "
        "  tag2 := g2.Tag(); "
        "end; "
        "Console.WriteLine(tag1 * 100 + tag2 * 10 + R._n); return 0;"), "441");
}
