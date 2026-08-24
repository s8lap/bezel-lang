// Codegen — interface-typed collection indexer `IList<T>[i]` get / set
// (issue #401, stdlib §4.1 / §4.2 / §4.3).
//
// The runtime-backed heap-collection indexer (#388) keyed on the CONCRETE
// collection type (Array / OwningArray) via isCollectionTypeName. When the
// receiver is one of the list INTERFACES the indexer was not lowered:
//   * a read returned garbage (it fell to the inline-array GEP path), and
//   * a write was silently dropped.
//
// The list interfaces that declare an int-indexer, per the stdlib:
//   * IReadOnlyList<out T>  (§4.1): `readonly property [i: int]: T;`
//                            get-only; covariant; the read borrows the slot.
//   * IList<T>              (§4.1): `property [i: int]: T;`
//                            get + set; borrowing; the setter stores a borrow,
//                            NO disposal of the previous slot (Array borrows).
//   * IOwningList<T>         (§4.2): `property [i: int]: T { get; set(owned); }`
//                            get + owned-taking set; the setter DISPOSES the
//                            previous slot value before installing the new one.
// (No collection INTERFACE declares a key-indexer: Dictionary<K,V> implements
//  only ICollection<DictionaryEntry<K,V>>, which has no indexer — so dictionary
//  key-indexing through an interface is intentionally not covered, §4.8.)
//
// Per §4.3, IReadOnlyList<T> is the shared read-only view of BOTH borrowing
// (Array<T>: IList<T>) and owning (OwningArray<T>: IOwningList<T>) array-backed
// collections. All three interfaces are array-backed at runtime (bz_array_t*),
// so the interface receiver holds the same runtime pointer as the concrete
// collection; the indexer get/set lower against the same bz_array_* runtime.
//
// Owning tests mirror collection_indexer_test.cpp: an element destructor bumps a
// static counter observed via the exit code, proving the owning interface setter
// disposes the displaced element exactly once; an LSan leg (when the toolchain is
// present) proves no leak / no double-free.

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
static const std::filesystem::path& wtDir401lsan() {
    return wtScratchDir("wt-401lsan");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}


// LeakSanitizer verification (modeled on collection_indexer_test.cpp). Returns:
//   0  — clean: no leak, no use-after-free, no double-free.
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir401lsan();
    std::string stem = unique_stem("bezel_401_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
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
// IReadOnlyList<T> — §4.1 get-only indexer over an array-backed collection.
// ---------------------------------------------------------------------------

// §4.1 / §4.3: an Array<int> viewed as IReadOnlyList<int> reads element i through
// the interface indexer. `+ 0` defeats the borrow-escape guard so the bare read
// reaches the return. Load-bearing: before the fix the interface read fell to the
// inline-array path and returned GARBAGE (0), not 20.
BEZEL_TEST(interface_indexer_401, ireadonlylist_int_get_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var xs: IReadOnlyList<int> := a; "
        "  var x := xs[1] + 0; "
        "  return x; "                                   // 20
        "end Main; end C;"), 20);
}

// §4.1 / §4.3: an OwningArray viewed as IReadOnlyList<T> is the SHARED read-only
// view of an owning collection (§4.3: the two hierarchies share IReadOnlyList).
// Reading through it borrows the slot — the array keeps ownership; the owner
// disposes both elements at scope end. Distinct elements read via the interface.
BEZEL_TEST(interface_indexer_401, ireadonlylist_over_owned_get_4_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R(5))); a.Add(move (new R(9))); "
        "  var xs: IReadOnlyList<R> := a; "
        "  return xs[1].V - xs[0].V; "                   // 9 - 5 = 4
        "end Main; end C;"), 4);
}

// ---------------------------------------------------------------------------
// IList<T> — §4.1 get + borrowing set over Array<T>.
// ---------------------------------------------------------------------------

// §4.1: an Array<int> viewed as IList<int> reads through the interface indexer.
BEZEL_TEST(interface_indexer_401, ilist_int_get_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); a.Add(8); a.Add(9); "
        "  var xs: IList<int> := a; "
        "  var x := xs[2] + 0; "
        "  return x; "                                   // 9
        "end Main; end C;"), 9);
}

// §4.1: `xs[i] := v` through an IList<int> view stores into the slot; reading the
// CONCRETE array back proves the write reached the same backing. Load-bearing:
// before the fix the interface setter was silently dropped and Get returned 20.
BEZEL_TEST(interface_indexer_401, ilist_int_set_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var xs: IList<int> := a; "
        "  xs[1] := 99; "
        "  return a.Get(1); "                            // 99
        "end Main; end C;"), 99);
}

// §4.1: set then read back THROUGH the interface (round-trip), no concrete calls.
BEZEL_TEST(interface_indexer_401, ilist_int_set_then_get_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var xs: IList<int> := a; "
        "  xs[0] := 7; xs[1] := 8; "
        "  var s := xs[0] + xs[1]; "
        "  return s; "                                   // 7 + 8 = 15
        "end Main; end C;"), 15);
}

// §4.1 / §3: a string-element IList<string> round-trips a pointer slot; .Length
// on the read value proves the right string reached the slot.
BEZEL_TEST(interface_indexer_401, ilist_string_set_get_4_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"x\"); a.Add(\"yy\"); "
        "  var xs: IList<string> := a; "
        "  xs[0] := \"zzzzz\"; "
        "  var n := xs[0].Length + xs[1].Length; "       // 5 + 2 = 7
        "  return n; "
        "end Main; end C;"), 7);
}

// §4.1 / §4.4: a borrowing IList<T> set does NOT dispose the previous slot value
// (the array borrows; element disposal is the caller's). The displaced element's
// destructor never fires from the set — `holder` owns both R instances and
// disposes them at its own scope end. Count read right after the interface set
// is 0 (no disposal by the set). Load-bearing: a borrowing interface set that
// wrongly used the disposing path would free a borrowed element (double-free).
BEZEL_TEST(interface_indexer_401, ilist_set_no_dispose_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var holder := new OwningArray<R>(); "
        "  holder.Add(move (new R())); holder.Add(move (new R())); "
        "  using var a := new Array<R>(); "
        "  a.Add(holder.Get(0)); "
        "  var xs: IList<R> := a; "
        "  xs[0] := holder.Get(1); "                     // borrow-replace: no dispose
        "  return R._n; "                                // 0 (set disposed nothing)
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// IOwningList<T> — §4.2 get + owned-taking set over OwningArray<T>. The setter
// DISPOSES the displaced element exactly once before installing the new one.
// ---------------------------------------------------------------------------

// §4.2: an OwningArray viewed as IOwningList<T> reads element i through the
// interface indexer (borrowing read). Two distinct elements differ.
BEZEL_TEST(interface_indexer_401, iownedlist_get_4_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R(3))); a.Add(move (new R(40))); "
        "  var xs: IOwningList<R> := a; "
        "  return xs[0].V + xs[1].V; "                   // 3 + 40 = 43
        "end Main; end C;"), 43);
}

// §4.2: "the indexer setter takes ownership and disposes the previous slot
// value." `xs[0] := move (new R())` through an IOwningList<R> view disposes the
// element previously at slot 0 EXACTLY once. Encoded: <count right after the set>
// * 10 + <count after the owning array's scope-end Dispose>. The set disposes 1
// (displaced); scope-end disposes the other 2 (new slot-0 element + slot-1) → 13.
// Load-bearing: before the fix the interface setter was a no-op, so the displaced
// element was never disposed and the moved-in element leaked (count differs).
BEZEL_TEST(interface_indexer_401, iownedlist_set_disposes_previous_4_2) {
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
        "    var xs: IOwningList<R> := a; "
        "    xs[0] := move (new R()); "
        "    afterSet := R._n; "
        "  end; "
        "  return afterSet * 10 + R._n; "                // 1*10 + 3 = 13
        "end Main; end C;";
    expectDisposedAndLeakFree(src, 13);
}

// §4.2: the owning interface setter installs the new element so Count is
// unchanged and exactly one previous element was disposed. Count read after the
// replacing set proves the slot still holds one live element.
BEZEL_TEST(interface_indexer_401, iownedlist_set_keeps_count_4_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); "
        "  a.Add(move (new R())); a.Add(move (new R())); "
        "  var xs: IOwningList<R> := a; "
        "  xs[1] := move (new R()); "
        "  return a.Count * 10 + R._n; "                 // count 2, one disposed → 21
        "end Main; end C;"), 21);
}

// ---------------------------------------------------------------------------
// Bounds / pass-through scenarios.
// ---------------------------------------------------------------------------

// §4.1 / §31.2: the interface indexer read bounds-checks the index and raises
// IndexOutOfRangeException when out of range, exactly as the concrete path. The
// throw is caught (non-const index, no compile-time elision); the body's read is
// NOT reached. Load-bearing: before the fix the interface read fell to the
// inline-array path (no heap bounds check) and read out-of-bounds memory.
BEZEL_TEST(interface_indexer_401, interface_indexer_out_of_range_throws_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  var xs: IReadOnlyList<int> := a; "
        "  var i := 5; "
        "  try "
        "    var x := xs[i] + 0; "
        "    return x; "                                 // unreachable
        "  catch IndexOutOfRangeException => return 42; "
        "  end; "
        "  return -1; "
        "end Main; end C;"), 42);
}

// §4.3: a method that accepts IReadOnlyList<T> (the canonical "read elements
// without taking ownership" parameter, per the §4.3 example) indexes the
// parameter. Calling it with a concrete Array<T> upcasts; the indexer lowers
// against the interface parameter type. Sums two elements read via the param.
BEZEL_TEST(interface_indexer_401, ireadonlylist_param_get_4_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Sum2(xs: IReadOnlyList<int>): int "
        "    return xs[0] + xs[1]; "
        "  end Sum2; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(11); a.Add(31); a.Add(99); "
        "    return Sum2(a); "                           // 11 + 31 = 42
        "  end Main; "
        "end C;"), 42);
}
