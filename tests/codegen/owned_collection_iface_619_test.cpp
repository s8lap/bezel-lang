// Codegen — interface-typed elements in owning collections are disposed
// (issue #619, §28.11 owned column, §14.4).
//
// §28.11 (spec table, owned column): "class / interface type → disposed".
// §14.4: the owning family admits both class AND interface element types.
//
// Prior to fix #619 the owning-collection element-policy helpers checked for
// Class only; an OwningArray<IFoo> or OwningDictionary<K,IFoo> would silently
// skip element disposal (the backing storage was freed but no destructor ran).
//
// Test structure:
//   * An interface ICounter (one method Count(): int).
//   * Two sealed implementing classes ImplA and ImplB, each with a static
//     destructor counter (_nA, _nB) and a shared total counter (_n).
//   * All collection operations exercised with interface-typed elements.
//   * Reverse-insertion order for OwningArray (§28.11 ordered collections).
//   * OwningDictionary: scope-end disposal, Remove, indexer-overwrite, Clear.
//   * Move out (PopAt equivalent via move arr[i]) leaves remaining disposal
//     correct.
//   * LSan-backed native tests for leak/UAF/double-free assurance.

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
static const std::filesystem::path& wtDir619() {
    return wtScratchDir("wt-619");
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification (mirrors owned_dict_disposal_test.cpp pattern).
// Returns 0=clean, 1=sanitizer-problem, -1=toolchain-unavailable/SKIP.
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir619();
    std::string stem = unique_stem("bezel_619_lsan");
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// stdout variant for observable values > 255 (the 8-bit exit-code ceiling): the
// program prints its result via Console.WriteLine and returns 0; the value is
// asserted on captured stdout (runAotOut) while the LSan leg still runs the same
// source for leak/double-free coverage.
void expectDisposedOutAndLeakFree(const std::string& source,
                                  const std::string& expectedStdout) {
    BEZEL_EXPECT_EQ(runAotOut(source), expectedStdout);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ---------------------------------------------------------------------------
// Shared test preamble: interface ICounter, two sealed implementing classes.
//
// ImplA._nA and ImplB._nB are per-class destructor counters.
// ICounter._n is the TOTAL (bumped by both destructors) — used for simple
// "N elements disposed" assertions.
// ImplA._log / ImplB._log are ORDER logs (each destructor appends its id via
// _log := _log * 10 + id) — used for reverse-insertion-order assertions.
// ---------------------------------------------------------------------------

// preamble: interface + two sealed impls.  Returns a string to prepend to Main.
const char* kPreamble =
    "module M; "
    "interface ICounter "
    "  method Count(): int; "
    "end ICounter; "
    "sealed class ImplA : ICounter "
    "  public static _nA: int := 0; "
    "  public static _n:  int := 0; "
    "  public static _log: int := 0; "
    "  private _id: int := 0; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Count(): int return _id; end Count; "
    "  destructor "
    "    _nA := _nA + 1; "
    "    _n  := _n  + 1; "
    "    _log := _log * 10 + _id; "
    "  end destructor; "
    "end ImplA; "
    "sealed class ImplB : ICounter "
    "  public static _nB: int := 0; "
    "  public static _n:  int := 0; "
    "  public static _log: int := 0; "
    "  private _id: int := 0; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Count(): int return _id; end Count; "
    "  destructor "
    "    _nB := _nB + 1; "
    "    _n  := _n  + 1; "
    "    _log := _log * 10 + _id; "
    "  end destructor; "
    "end ImplB; ";

// Build a complete program from preamble + main class body.
// _n is tracked in ImplA (shared counter bumped by both classes).
std::string prog(const std::string& mainBody) {
    return std::string(kPreamble) +
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ===========================================================================
// OwningArray<ICounter> — §28.11 / §14.4
// ===========================================================================

// §28.11 owned column / §14.4: OwningArray<IFace>.Dispose() (scope-end) must
// dispose each interface-typed element via vtable dispatch. Two elements (one
// ImplA, one ImplB) → both destructors run → ImplA._n == 1, ImplB._n == 1,
// total disposed == 2. Before #619 the elements leaked → ImplA._n + ImplB._n
// == 0.
// Encode: ImplA._nA * 10 + ImplB._nB; expected 11.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_elements_disposed_on_scope_end) {
    expectDisposedAndLeakFree(prog(
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplB(2))); "
        "end; "
        "return ImplA._nA * 10 + ImplB._nB;"), 11);
}

// §28.11: OwningArray<ICounter> disposes ALL elements exactly once — no leak,
// no double-dispose. Three elements (3 ImplA instances) → _n == 3.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_all_three_disposed_once) {
    expectDisposedAndLeakFree(prog(
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplA(2))); "
        "  a.Add(move (new ImplA(3))); "
        "end; "
        "return ImplA._n;"), 3);
}

// §28.11 (ordered collection, reverse-insertion order): Add ImplA(1), ImplA(2),
// ImplA(3); dispose → order is 3→2→1 → _log == 321.
// Before #619 elements were never disposed → _log == 0.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_disposes_reverse_insertion_order) {
    // _log packs the disposal order 3->2->1 as 321 > 255 → print + runAotOut.
    expectDisposedOutAndLeakFree(prog(
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplA(2))); "
        "  a.Add(move (new ImplA(3))); "
        "end; "
        "Console.WriteLine(ImplA._log); return 0;"), "321");
}

// §28.11: OwningArray<IFace>.Clear() disposes every interface element.
// Two elements added; Clear() disposes both (count 2); scope-end finds empty
// array and disposes nothing more (still 2). Encode: afterClear*10+final → 22.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_clear_disposes_all_no_redispose) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterClear := -1; "
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplA(2))); "
        "  a.Clear(); "
        "  afterClear := ImplA._n; "
        "end; "
        "return afterClear * 10 + ImplA._n;")), 22);
}

// §28.11: OwningArray<IFace>.RemoveAt(i) disposes the removed element only.
// Three elements; RemoveAt(1) disposes the middle one (count 1); scope-end
// disposes the remaining two (count 3). Encode: afterRemove*10+final → 13.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_remove_at_disposes_only_removed) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterRemove := -1; "
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplA(2))); "
        "  a.Add(move (new ImplA(3))); "
        "  a.RemoveAt(1); "
        "  afterRemove := ImplA._n; "
        "end; "
        "return afterRemove * 10 + ImplA._n;")), 13);
}

// §28.11: move out of an OwningArray<IFace> slot via `move a[i]` extracts the
// element WITHOUT disposing it (caller takes ownership). The remaining element
// IS disposed when the array goes out of scope. Two elements; move index 0 out
// (no disposal yet), then scope ends disposing the one remaining element.
// Expected: _n == 1 after scope end.
BEZEL_TEST(owned_coll_iface_619_spec_28_11_14_4,
           owned_array_iface_move_out_leaves_remaining_disposed) {
    BEZEL_EXPECT_EQ(run(prog(
        "var movedOut: ICounter? := null; "
        "do "
        "  using var a := new OwningArray<ICounter>(); "
        "  a.Add(move (new ImplA(1))); "
        "  a.Add(move (new ImplA(2))); "
        "  movedOut := move a[0]; "   // takes ownership of index 0
        "end; "
        // scope-end: array has 1 element (index 1, ImplA(2)) → disposed → _n=1
        // movedOut still alive here (not using var → no dispose until end of fn)
        "return ImplA._n;")), 1);
}

// ===========================================================================
// OwningDictionary<string, ICounter> — §28.11 / §14.4
// ===========================================================================

// §28.11 / §4.9: OwningDictionary<string, IFace> disposes surviving values at
// scope end. Two values (ImplA and ImplB) both survive → count 2.
// Before #619 the values leaked → count 0.
// Encode: ImplA._nA * 10 + ImplB._nB → 11.
BEZEL_TEST(owned_dict_iface_619_spec_28_11_14_4,
           owned_dict_str_iface_values_disposed_on_scope_end) {
    expectDisposedAndLeakFree(prog(
        "var inside := -1; "
        "do "
        "  using var d := new OwningDictionary<string, ICounter>(); "
        "  d.Add(\"a\", move (new ImplA(1))); "
        "  d.Add(\"b\", move (new ImplB(2))); "
        "  inside := ImplA._nA + ImplB._nB; "  // 0 (nothing disposed yet)
        "end; "
        "return inside * 100 + ImplA._nA * 10 + ImplB._nB;"), 11);
}

// §28.11 / §4.9: OwningDictionary<string, IFace>.Clear() disposes all values.
// Two survivors; Clear() disposes both (count 2 at afterClear); scope-end
// finds an empty dict (count still 2). Encode: afterClear*10+final → 22.
BEZEL_TEST(owned_dict_iface_619_spec_28_11_14_4,
           owned_dict_str_iface_clear_disposes_values) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterClear := -1; "
        "do "
        "  using var d := new OwningDictionary<string, ICounter>(); "
        "  d.Add(\"a\", move (new ImplA(1))); "
        "  d.Add(\"b\", move (new ImplA(2))); "
        "  d.Clear(); "
        "  afterClear := ImplA._n; "
        "end; "
        "return afterClear * 10 + ImplA._n;")), 22);
}

// §4.9: OwningDictionary<string, IFace>.Remove(k) disposes the removed value.
// Add two values; Remove("a") disposes that value (count 1); scope-end disposes
// the remaining "b" (count 2). Encode: afterRemove*10+final → 12.
BEZEL_TEST(owned_dict_iface_619_spec_28_11_14_4,
           owned_dict_str_iface_remove_disposes_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var afterRemove := -1; "
        "do "
        "  using var d := new OwningDictionary<string, ICounter>(); "
        "  d.Add(\"a\", move (new ImplA(1))); "
        "  d.Add(\"b\", move (new ImplA(2))); "
        "  d.Remove(\"a\"); "
        "  afterRemove := ImplA._n; "
        "end; "
        "return afterRemove * 10 + ImplA._n;")), 12);
}

// §4.9: indexer-overwrite on OwningDictionary<string, IFace> disposes the
// displaced value. Overwrite "a" → first ImplA(1) is disposed (count 1);
// at scope end the two survivors ("a"-second, "b") are disposed (count 3).
// The displaced value must NOT be disposed again (no double-free).
// Encode: afterDisplace*10+final → 13. LSan leg: no double-free.
BEZEL_TEST(owned_dict_iface_619_spec_28_11_14_4,
           owned_dict_str_iface_indexer_overwrite_disposes_displaced) {
    expectDisposedAndLeakFree(prog(
        "var afterDisplace := -1; "
        "do "
        "  using var d := new OwningDictionary<string, ICounter>(); "
        "  d[\"a\"] := move (new ImplA(1)); "
        "  d[\"b\"] := move (new ImplA(2)); "
        "  d[\"a\"] := move (new ImplA(3)); "   // displaces ImplA(1)
        "  afterDisplace := ImplA._n; "        // 1 (only displaced was disposed)
        "end; "
        "return afterDisplace * 10 + ImplA._n;"), 13);
}

// ===========================================================================
// OwningDictionary<int, ICounter> — generic-key path (§4.9 / D3)
// ===========================================================================

// §28.11 / §4.9: generic-key path must also dispose interface-valued elements.
// Two values survive to scope end → count 2.
BEZEL_TEST(owned_dict_iface_619_spec_28_11_14_4,
           owned_dict_int_iface_values_disposed_on_scope_end) {
    expectDisposedAndLeakFree(prog(
        "var inside := -1; "
        "do "
        "  using var d := new OwningDictionary<int, ICounter>(); "
        "  d.Add(1, move (new ImplA(1))); "
        "  d.Add(2, move (new ImplA(2))); "
        "  inside := ImplA._n; "  // 0 (nothing disposed yet)
        "end; "
        "return inside * 10 + ImplA._n;"), 2);
}

// ===========================================================================
// OwningLinkedList<ICounter> — §28.11 ordered collection / §14.4
// ===========================================================================

// §28.11: OwningLinkedList<IFace>.Dispose() disposes elements in reverse-
// insertion order. AddLast(ImplA(1)), AddLast(ImplA(2)), AddLast(ImplA(3))
// → dispose 3 first, then 2, then 1 → _log == 321.
BEZEL_TEST(owned_llist_iface_619_spec_28_11_14_4,
           owned_llist_iface_disposes_reverse_insertion_order) {
    // _log packs the disposal order 3->2->1 as 321 > 255 → print + runAotOut.
    expectDisposedOutAndLeakFree(prog(
        "do "
        "  using var ll := new OwningLinkedList<ICounter>(); "
        "  ll.AddLast(move (new ImplA(1))); "
        "  ll.AddLast(move (new ImplA(2))); "
        "  ll.AddLast(move (new ImplA(3))); "
        "end; "
        "Console.WriteLine(ImplA._log); return 0;"), "321");
}

// §28.11: all interface-typed elements in OwningLinkedList disposed exactly once.
BEZEL_TEST(owned_llist_iface_619_spec_28_11_14_4,
           owned_llist_iface_all_elements_disposed_once) {
    expectDisposedAndLeakFree(prog(
        "do "
        "  using var ll := new OwningLinkedList<ICounter>(); "
        "  ll.AddLast(move (new ImplA(1))); "
        "  ll.AddLast(move (new ImplB(2))); "
        "end; "
        "return ImplA._nA * 10 + ImplB._nB;"), 11);
}

// ===========================================================================
// #819 / §14.4: a RECORD implementing an interface, added to an
// OwningArray<IFace>, must be HEAP-BOXED so a vtable sits at word 0. Before the
// fix the raw record bytes were stored in the pointer-sized slot and the element
// disposer treated the first field as a vtable pointer → SIGSEGV on disposal.
// (The class-element case above already worked; records are the gap.)
// ===========================================================================

const char* kRecordIfaceProg =
    "module M; "
    "interface IFoo method Foo(): int; end IFoo; "
    "record R(n: int) : IFoo "
    "  public method Foo(): int => n; "
    "end R; ";

// Count after adding a record-boxed element: 1 (was a SIGSEGV on scope-end
// disposal).
BEZEL_TEST(owned_coll_iface_819_record_box, record_added_count) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRecordIfaceProg) +
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<IFoo>(); "
        "  arr.Add(new R(5)); "
        "  return arr.Count; "                         // 1
        "end Main; end C;"), 1);
}

// Interface dispatch through the boxed record's itable returns the field value.
BEZEL_TEST(owned_coll_iface_819_record_box, record_dispatch_through_box) {
    BEZEL_EXPECT_EQ(run(
        std::string(kRecordIfaceProg) +
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<IFoo>(); "
        "  arr.Add(new R(5)); "
        "  return arr[0].Foo(); "                       // 5
        "end Main; end C;"), 5);
}

// Disposal of an OwningArray<IFoo> holding boxed records is leak-free and
// crash-free (the boxes are reclaimed exactly once).
BEZEL_TEST(owned_coll_iface_819_record_box, record_elements_disposed_leak_free) {
    const std::string p =
        std::string(kRecordIfaceProg) +
        "class C public static method Main(): int "
        "  do "
        "    using var arr := new OwningArray<IFoo>(); "
        "    arr.Add(new R(1)); "
        "    arr.Add(new R(2)); "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(p), 0);
    int lsan = runUnderLeakSanitizer(p);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Regression guard: a CLASS implementing the interface still works (boxForTarget
// is a no-op for an already-pointer element). Dispatch returns 7.
BEZEL_TEST(owned_coll_iface_819_record_box, class_element_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IFoo method Foo(): int; end IFoo; "
        "sealed class K : IFoo "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Foo(): int => _v; "
        "  destructor end destructor; "
        "end K; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<IFoo>(); "
        "  arr.Add(new K(7)); "
        "  return arr[0].Foo(); "                       // 7
        "end Main; end C;"), 7);
}
