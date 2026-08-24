// Codegen + sema — #549: ArraySpan<T> / ReadOnlyArraySpan<T>
//
// These are borrow-bearing record types defined in stdlib §4.15.  Each view
// borrows a contiguous range of an Array<T>; the span must NOT free the array.
//
// Spec reference: Bezel_Standard_Library_0_130.md §4.15.
//
// Tests (all named for §4.15):
//   §4.15 AsSpan() (0-arg) → Length matches array.Count
//   §4.15 AsSpan(start, length) → partial view
//   §4.15 indexer read  span[i] → correct element
//   §4.15 indexer write span[i] := v → element is updated in source array
//   §4.15 IsEmpty  on empty vs non-empty span
//   §4.15 Slice(start, length) → sub-span
//   §4.15 AsReadOnly() → ReadOnlyArraySpan<T> with same length
//   §4.15 AsReadOnlySpan() construction directly
//   §4.15 IndexOf(v) on span
//   §4.15 Contains(v) on span
//   §4.15 Fill(v) overwrites every slot
//   §4.15 CopyFrom(src) copies slot-by-slot
//   §4.15 CopyTo(dst) copies slot-by-slot
//   §4.15 Reverse() reverses in place (and in source array)
//   §4.15 SequenceEqual (same / different span)
//   §4.15 for-in iteration over span
//   §4.15 ASan: span over a 'using var' array does NOT free array at span end
//   §4.15 ReadOnly: indexer setter rejected at compile time

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
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
static const std::filesystem::path& wtDir549() {
    return wtScratchDir("wt-549");
}

// Run the JIT pipeline; return exit code, or -999 on compile / JIT failure.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff sema rejected `src` with at least one error diagnostic.
bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// ---- ASan leg (pattern from array_reversed_sorted_472_test.cpp) --------------
int lowerToObject(const std::string& source, const std::string& objPath) {
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
    return 0;
}

int runUnderAsan(const std::string& source, int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    auto dir = wtDir549();
    std::string stem = unique_stem("bezel_549");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return -1;
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return 1;
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;
    return (code == expectedExitCode) ? 0 : 1;
}

// Assert: JIT-correct AND ASan-clean.  Skips ASan leg when toolchain absent.
void expectValueAndAsanClean(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int a = runUnderAsan(source, expectedExitCode);
    if (a == -1) return;
    BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// ===========================================================================
// §4.15 AsSpan() 0-arg: full array view — Length equals array Count.
// ===========================================================================

// §4.15: arr.AsSpan().Length == arr.Count (3-element array → Length=3).
BEZEL_TEST(array_span_549, asspan_0arg_length_equals_count_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  return sp.Length; "  // 3
        "end Main; end C;", 3);
}

// §4.15: AsSpan() on empty array → Length=0.
BEZEL_TEST(array_span_549, asspan_0arg_empty_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var sp := a.AsSpan(); "
        "  return sp.Length; "  // 0
        "end Main; end C;", 0);
}

// §4.5 (#1022): OwningArray<T> does NOT expose AsSpan — its elements are uniquely
// owned, so a span view (writable, can overwrite owned slots) is not provided.
// Before #1022 this was accepted and codegen SILENTLY miscompiled it to a zeroed
// span (sp[i] == 0 for every i); now it is a clean compile error.
BEZEL_TEST(array_span_549, ownedarray_asspan_rejected_1022) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; "
        "class R end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(new R()); "
        "  var sp := a.AsSpan(); "
        "  return 0; "
        "end Main; end C;"));
}

// §4.5 (#1022): OwningArray<T>.AsReadOnlySpan() is likewise rejected.
BEZEL_TEST(array_span_549, ownedarray_asreadonlyspan_rejected_1022) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; "
        "class R end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(new R()); "
        "  var sp := a.AsReadOnlySpan(); "
        "  return 0; "
        "end Main; end C;"));
}

// §4.5 (#1022) regression: the borrowing Array<T>.AsSpan() is UNAFFECTED — it
// still compiles and indexes correctly (a[1] via the span == 8).
BEZEL_TEST(array_span_549, array_asspan_index_unaffected_1022) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); a.Add(8); a.Add(9); "
        "  var sp := a.AsSpan(); "
        "  return sp[1]; "  // 8 (was the silent-0 bug only on OwningArray)
        "end Main; end C;", 8);
}

// §4.5 (#1022): the non-owning copy-form materializers ToArray/ToSet/ToDictionary
// are rejected on an OWNING collection — copying would double-own its uniquely
// owned elements. (Before the fix they were silently accepted and codegen
// mis-reclaimed the materializer backing.) §4.5 L1202: "There is no ToArray() on
// OwningArray<T> that produces a non-owning copy."
BEZEL_TEST(array_span_549, ownedarray_toarray_rejected_1022) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(new R()); "
        "  _ := a.ToArray(); "
        "  return 0; "
        "end Main; end C;"));
}

BEZEL_TEST(array_span_549, ownedarray_toset_rejected_1022) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class R end R; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<R>(); a.Add(new R()); "
        "  _ := a.ToSet(); "
        "  return 0; "
        "end Main; end C;"));
}

BEZEL_TEST(array_span_549, ownedarray_todictionary_rejected_1022) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; "
        "import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); a.Add(1); a.Add(2); "
        "  _ := a.ToDictionary(lambda(x) => x); "
        "  return 0; "
        "end Main; end C;"));
}

// §4.5 (#1022) regression: a LINQ query result over an owning collection has an
// IEnumerable receiver, so its ToArray() materializer is the borrowing copy-form
// and stays VALID (only a DIRECT owning-collection receiver is rejected).
// Migrated from OwningArray<int> to OwningArray<Cell> per stdlib §4.2/#1327.
BEZEL_TEST(array_span_549, ownedarray_linq_then_toarray_valid_1022) {
    expectValueAndAsanClean(
        "module M; "
        "import System.Collections.Generic; "
        "class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; end Cell; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(7)); a.Add(new Cell(8)); a.Add(new Cell(9)); "
        "  using var e: Array<Cell> := a.Where(lambda(x) => x.V > 7).ToArray(); "
        "  return e.Length; "  // 2 (8 and 9)
        "end Main; end C;", 2);
}

// §22.5 / §4.x (#1103-B / #920): a method call on a direct owning-collection
// receiver whose name is outside the owning surface (nor a universal Object
// member / LINQ operator / enumerable protocol) is rejected — `TakeAt`
// (nonexistent), an arbitrary `Frobnicate` (was silently accepted + mis-lowered
// to a wrong value).
BEZEL_TEST(array_span_549, ownedarray_unknown_method_takeat_rejected_1103b) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); a.Add(7); "
        "  return a.TakeAt(0); "
        "end Main; end C;"));
}

BEZEL_TEST(array_span_549, ownedarray_unknown_method_arbitrary_rejected_1103b) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); a.Add(7); "
        "  a.Frobnicate(); "
        "  return 0; "
        "end Main; end C;"));
}

// #1103-B regression: the real owning-collection surface stays accepted — the
// element accessor `Get`, `Count`, `RemoveAt`, and a borrowing LINQ `Where`.
BEZEL_TEST(array_span_549, ownedarray_real_surface_accepted_1103b) {
    expectValueAndAsanClean(
        "module M; import System.Collections.Generic; "
        "class Cell public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; end Cell; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(7)); a.Add(new Cell(9)); "
        "  var hi := a.Where(lambda(c) => c._v > 8).Count(); "  // 1
        "  return a.Get(1)._v + a.Count + hi; "           // 9 + 2 + 1 = 12
        "end Main; end C;", 12);
}

// ===========================================================================
// §4.15 AsSpan(start, length): partial view.
// ===========================================================================

// §4.15: AsSpan(1, 2) on [10,20,30,40] → Length=2.
BEZEL_TEST(array_span_549, asspan_start_length_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(1, 2); "
        "  return sp.Length; "  // 2
        "end Main; end C;", 2);
}

// §4.15: AsSpan(1) on [10,20,30] → Length=2 (from offset to end).
BEZEL_TEST(array_span_549, asspan_start_only_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(1); "
        "  return sp.Length; "  // 2
        "end Main; end C;", 2);
}

// ===========================================================================
// §4.15 Indexer read: span[i] returns correct element.
// ===========================================================================

// §4.15: span[0] on full-array span returns first element.
// [10,20,30]; span[2]=30.
BEZEL_TEST(array_span_549, indexer_read_full_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  return sp[2]; "  // 30
        "end Main; end C;", 30);
}

// §4.15: span[i] on a partial span indexes into the correct source offset.
// [10,20,30,40]; sp=AsSpan(1,2) covers [20,30]; sp[0]=20, sp[1]=30 → 20*7+30=170.
BEZEL_TEST(array_span_549, indexer_read_partial_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(1, 2); "
        "  return sp[0] * 7 + sp[1]; "  // 20*7+30=170
        "end Main; end C;", 170 % 256);
}

// ===========================================================================
// §4.15 Indexer write: span[i] := v stores into source array.
// ===========================================================================

// §4.15: span[1] := 77 on full array → a.Get(1)=77.
BEZEL_TEST(array_span_549, indexer_write_mutates_source_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  sp[1] := 77; "
        "  return a.Get(1); "  // 77
        "end Main; end C;", 77);
}

// §4.15: write through partial span at sp[0] updates source array at offset Start.
// [10,20,30,40]; sp=AsSpan(1,2); sp[0]:=55 → a.Get(1)=55 → 55.
BEZEL_TEST(array_span_549, indexer_write_partial_span_offset_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(1, 2); "
        "  sp[0] := 55; "
        "  return a.Get(1); "  // 55
        "end Main; end C;", 55);
}

// ===========================================================================
// §4.15 IsEmpty property.
// ===========================================================================

// §4.15: IsEmpty is true on a zero-length span; false on a non-empty span.
// Return 1 if empty, 0 otherwise (for empty span); and 0 if empty, 1 otherwise.
// Encode: empty → return 1; non-empty → return 0.
BEZEL_TEST(array_span_549, isempty_empty_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var sp := a.AsSpan(); "
        "  if sp.IsEmpty then return 1; end; "
        "  return 0; "
        "end Main; end C;", 1);
}

BEZEL_TEST(array_span_549, isempty_nonempty_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(7); "
        "  var sp := a.AsSpan(); "
        "  if sp.IsEmpty then return 1; end; "
        "  return 0; "
        "end Main; end C;", 0);
}

// ===========================================================================
// §4.15 Slice(start, length): sub-span construction.
// ===========================================================================

// §4.15: Slice(1,2) on a full span [10,20,30,40] → view [20,30]; Length=2.
BEZEL_TEST(array_span_549, slice_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(); "
        "  var sl := sp.Slice(1, 2); "
        "  return sl.Length; "  // 2
        "end Main; end C;", 2);
}

// §4.15: Slice(1,2)[0] should return element at original index 1.
// [10,20,30,40].AsSpan().Slice(1,2)[0]=20; Slice(1,2)[1]=30 → 20*7+30=170.
BEZEL_TEST(array_span_549, slice_indexer_read_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(); "
        "  var sl := sp.Slice(1, 2); "
        "  return sl[0] * 7 + sl[1]; "  // 20*7+30=170
        "end Main; end C;", 170 % 256);
}

// ===========================================================================
// §4.15 AsReadOnly(): converts ArraySpan<T> → ReadOnlyArraySpan<T>.
// ===========================================================================

// §4.15: AsReadOnly() on a 3-element span → ReadOnlyArraySpan<T> with Length=3.
BEZEL_TEST(array_span_549, as_readonly_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var sp := a.AsSpan(); "
        "  var ro := sp.AsReadOnly(); "
        "  return ro.Length; "  // 3
        "end Main; end C;", 3);
}

// §4.15: AsReadOnly()[i] reads correctly.
// [10,20,30].AsSpan().AsReadOnly()[2]=30.
BEZEL_TEST(array_span_549, as_readonly_indexer_read_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  var ro := sp.AsReadOnly(); "
        "  return ro[2]; "  // 30
        "end Main; end C;", 30);
}

// ===========================================================================
// §4.15 AsReadOnlySpan() direct construction from Array<T>.
// ===========================================================================

// §4.15: a.AsReadOnlySpan() → ReadOnlyArraySpan<T> with Length=a.Count.
BEZEL_TEST(array_span_549, as_readonly_span_direct_length_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(6); "
        "  var ro := a.AsReadOnlySpan(); "
        "  return ro.Length; "  // 2
        "end Main; end C;", 2);
}

// §4.15: a.AsReadOnlySpan()[i] reads element correctly.
// [5,6]; ro[0]=5, ro[1]=6 → 5*10+6=56.
BEZEL_TEST(array_span_549, as_readonly_span_direct_indexer_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(6); "
        "  var ro := a.AsReadOnlySpan(); "
        "  return ro[0] * 10 + ro[1]; "  // 56
        "end Main; end C;", 56);
}

// ===========================================================================
// §4.15 IndexOf(value): linear scan in span range.
// ===========================================================================

// §4.15: IndexOf finds value at correct span-local index.
// [10,20,30,20]; sp=AsSpan(); IndexOf(20)=1.
BEZEL_TEST(array_span_549, indexof_found_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(20); "
        "  var sp := a.AsSpan(); "
        "  return sp.IndexOf(20); "  // 1
        "end Main; end C;", 1);
}

// §4.15: IndexOf returns -1 when value not present.
// [10,20,30]; IndexOf(99)=-1 → encode as exit code by returning 0 when ==-1.
BEZEL_TEST(array_span_549, indexof_not_found_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  if sp.IndexOf(99) = -1 then return 7; end; "
        "  return 0; "
        "end Main; end C;", 7);
}

// §4.15: IndexOf on partial span uses span-local indexing (not array indexing).
// [10,20,30,20]; sp=AsSpan(2,2) covers [30,20]; IndexOf(20)=1.
BEZEL_TEST(array_span_549, indexof_partial_span_local_index_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(20); "
        "  var sp := a.AsSpan(2, 2); "
        "  return sp.IndexOf(20); "  // 1 (local index within [30,20])
        "end Main; end C;", 1);
}

// ===========================================================================
// §4.15 Contains(value).
// ===========================================================================

// §4.15: Contains returns true when value is present.
BEZEL_TEST(array_span_549, contains_true_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  if sp.Contains(20) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 1);
}

// §4.15: Contains returns false when value is absent.
BEZEL_TEST(array_span_549, contains_false_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  if sp.Contains(99) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 0);
}

// ===========================================================================
// §4.15 Fill(value): overwrites every slot in the span's range.
// ===========================================================================

// §4.15: Fill(5) on a 3-element span writes 5 to all slots.
// After Fill, a[0]=a[1]=a[2]=5.
BEZEL_TEST(array_span_549, fill_overwrites_all_slots_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  sp.Fill(5); "
        // Verify all three slots got 5: a[0]+a[1]+a[2] = 5+5+5 = 15
        "  return a.Get(0) + a.Get(1) + a.Get(2); "  // 5+5+5=15
        "end Main; end C;", 15);
}

// §4.15: Fill on partial span only mutates the span's range; elements outside remain.
// [10,20,30,40]; sp=AsSpan(1,2); Fill(0) → a=[10,0,0,40].
// Return a.Get(0)*1000 + a.Get(1)*100 + a.Get(2)*10 + a.Get(3) = 10040 % 256 = 56.
BEZEL_TEST(array_span_549, fill_partial_span_range_only_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(1, 2); "
        "  sp.Fill(0); "
        // a[0]=10, a[1]=0, a[2]=0, a[3]=40
        // Use a small encoding: a[0]*3 + a[1]*2 + a[2]*1 + a[3]*1 = 30+0+0+40=70
        "  return a.Get(0) * 3 + a.Get(1) * 2 + a.Get(2) + a.Get(3); "
        "end Main; end C;", 70);
}

// ===========================================================================
// §4.15 Reverse(): reverses slot ordering in place.
// ===========================================================================

// §4.15: Reverse() on a 3-element full span: [10,20,30] → [30,20,10].
// After Reverse, a[0]=30.
BEZEL_TEST(array_span_549, reverse_inplace_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := a.AsSpan(); "
        "  sp.Reverse(); "
        "  return a.Get(0); "  // 30
        "end Main; end C;", 30);
}

// §4.15: Reverse() on a 4-element span: [10,20,30,40] → [40,30,20,10].
// Return a.Get(0)=40.
BEZEL_TEST(array_span_549, reverse_four_elements_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(); "
        "  sp.Reverse(); "
        "  return a.Get(0); "  // 40
        "end Main; end C;", 40);
}

// §4.15: Reverse() on a single-element span is a no-op.
BEZEL_TEST(array_span_549, reverse_single_element_noop_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(42); "
        "  var sp := a.AsSpan(); "
        "  sp.Reverse(); "
        "  return a.Get(0); "  // 42
        "end Main; end C;", 42);
}

// ===========================================================================
// §4.15 CopyFrom(source): copies slot-by-slot from ReadOnlyArraySpan<T>.
// ===========================================================================

// §4.15: CopyFrom copies all elements from source to destination span.
// src=[1,2,3], dst=[0,0,0]; dst.CopyFrom(src) → dst now reads [1,2,3].
// After copy, a=[1,2,3] → 1*49+2*7+3=66.
BEZEL_TEST(array_span_549, copyfrom_copies_elements_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var src := new Array<int>(); "
        "  src.Add(1); src.Add(2); src.Add(3); "
        "  using var dst := new Array<int>(); "
        "  dst.Add(0); dst.Add(0); dst.Add(0); "
        "  var dstSp := dst.AsSpan(); "
        "  var srcRo := src.AsReadOnlySpan(); "
        "  dstSp.CopyFrom(srcRo); "
        "  return dst.Get(0) * 49 + dst.Get(1) * 7 + dst.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// ===========================================================================
// §4.15 CopyTo(destination): copies slot-by-slot to destination ArraySpan<T>.
// ===========================================================================

// §4.15: CopyTo copies all elements into the destination span.
// src=[1,2,3]; dstSp:=dst.AsSpan(); srcRo.CopyTo(dstSp) → dst=[1,2,3].
// After copy, dst=[1,2,3] → 1*49+2*7+3=66.
BEZEL_TEST(array_span_549, copyto_copies_elements_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var src := new Array<int>(); "
        "  src.Add(1); src.Add(2); src.Add(3); "
        "  using var dst := new Array<int>(); "
        "  dst.Add(0); dst.Add(0); dst.Add(0); "
        "  var srcRo := src.AsReadOnlySpan(); "
        "  var dstSp := dst.AsSpan(); "
        "  srcRo.CopyTo(dstSp); "
        "  return dst.Get(0) * 49 + dst.Get(1) * 7 + dst.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// ===========================================================================
// §4.15 SequenceEqual: pairwise comparison.
// ===========================================================================

// §4.15: SequenceEqual returns true for two spans of identical elements.
BEZEL_TEST(array_span_549, sequence_equal_equal_spans_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); "
        "  b.Add(1); b.Add(2); b.Add(3); "
        "  var sa := a.AsSpan(); "
        "  var sb := b.AsSpan(); "
        "  if sa.SequenceEqual(sb) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 1);
}

// §4.15: SequenceEqual returns false for spans with different elements.
BEZEL_TEST(array_span_549, sequence_equal_different_spans_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var b := new Array<int>(); "
        "  b.Add(1); b.Add(2); b.Add(9); "
        "  var sa := a.AsSpan(); "
        "  var sb := b.AsSpan(); "
        "  if sa.SequenceEqual(sb) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 0);
}

// §4.15: SequenceEqual returns false when lengths differ.
BEZEL_TEST(array_span_549, sequence_equal_length_mismatch_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  using var b := new Array<int>(); "
        "  b.Add(1); b.Add(2); b.Add(3); "
        "  var sa := a.AsSpan(); "
        "  var sb := b.AsSpan(); "
        "  if sa.SequenceEqual(sb) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 0);
}

// §4.15: SequenceEqual with ReadOnlyArraySpan<T> overload.
BEZEL_TEST(array_span_549, sequence_equal_with_readonly_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(6); a.Add(7); "
        "  using var b := new Array<int>(); "
        "  b.Add(5); b.Add(6); b.Add(7); "
        "  var sa := a.AsSpan(); "
        "  var rob := b.AsReadOnlySpan(); "
        "  if sa.SequenceEqual(rob) then return 1; end; "
        "  return 0; "
        "end Main; end C;", 1);
}

// ===========================================================================
// §4.15 for-in loop over a span.
// ===========================================================================

// §4.15: for-in loop over span sums all elements.
// [1,2,3,4]; sum=10.
BEZEL_TEST(array_span_549, for_in_sum_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); "
        "  var sp := a.AsSpan(); "
        "  var sum := 0; "
        "  for var x in sp do "
        "    sum := sum + x; "
        "  end; "
        "  return sum; "  // 10
        "end Main; end C;", 10);
}

// §4.15: for-in over partial span iterates only span range.
// [10,20,30,40]; sp=AsSpan(1,2) covers [20,30]; sum=50.
BEZEL_TEST(array_span_549, for_in_partial_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "  var sp := a.AsSpan(1, 2); "
        "  var sum := 0; "
        "  for var x in sp do "
        "    sum := sum + x; "
        "  end; "
        "  return sum; "  // 20+30=50
        "end Main; end C;", 50);
}

// §4.15: for-in over ReadOnlyArraySpan<T> (via AsReadOnlySpan).
// [1,2,3]; sum=6.
BEZEL_TEST(array_span_549, for_in_readonly_span_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  var ro := a.AsReadOnlySpan(); "
        "  var sum := 0; "
        "  for var x in ro do "
        "    sum := sum + x; "
        "  end; "
        "  return sum; "  // 6
        "end Main; end C;", 6);
}

// ===========================================================================
// §4.15 ASan: span borrows array — span end does NOT free the array.
// ===========================================================================

// §4.15 borrow lifetime: the span is a value-type record; its disposal must NOT
// free the source array.  The source array remains accessible after the span
// goes out of scope. After the span is done, the array still holds [1,2,3].
// ASan detects any use-after-free or double-free.
BEZEL_TEST(array_span_549, asan_span_does_not_free_array_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  do "
        "    var sp := a.AsSpan(); "
        "    var x := sp[0]; "
        "  end; "
        // span 'sp' is gone now; array 'a' must still be alive
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// §4.15 ASan: ReadOnlyArraySpan also does not free source array.
BEZEL_TEST(array_span_549, asan_readonly_span_does_not_free_array_spec_4_15) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(4); a.Add(5); "
        "  do "
        "    var ro := a.AsReadOnlySpan(); "
        "    var x := ro[0]; "
        "  end; "
        "  return a.Get(0) * 10 + a.Get(1); "  // 4*10+5=45
        "end Main; end C;", 45);
}

// ===========================================================================
// §4.15 "Caveat: array mutation during span lifetime" (#1158): the span getter
// is `return Source[Start + i]`, which routes through the array's own
// Count-checked indexer. If the array shrinks below Start+Length after the span
// was constructed, an access whose absolute index is past the new Count throws
// IndexOutOfRangeException (NOT a stale read of the old backing slot).
// ===========================================================================

// §4.15: [10,20,30,40]; sp=AsSpan(0,4); RemoveAt(3) (Count→3); sp[3] passes the
// span Length check (3 < 4) but Start+3=3 >= live Count 3 → throw. Before #1158
// the getter called bz_array_get directly and returned the stale 40.
BEZEL_TEST(array_span_549, indexer_throws_after_array_shrinks_spec_4_15_1158) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); "
        "  var sp := arr.AsSpan(0, 4); "
        "  arr.RemoveAt(3); "          // Count now 3
        "  try "
        "    return sp[3]; "            // stale 40 before fix; now throws
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// §4.15 (#1158): an in-bounds access after the shrink still reads correctly —
// sp[2] (Start+2=2 < Count 3) returns 30. Guards against over-eager rejection.
BEZEL_TEST(array_span_549, indexer_inbounds_after_shrink_ok_spec_4_15_1158) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); "
        "  var sp := arr.AsSpan(0, 4); "
        "  arr.RemoveAt(3); "          // Count now 3
        "  return sp[2]; "             // 30 (still in range)
        "end Main; end C;", 30);
}

// §4.15 (#1158): ReadOnlyArraySpan<T> getter behaves identically — sp[3] after
// the shrink throws IndexOutOfRangeException.
BEZEL_TEST(array_span_549, readonly_indexer_throws_after_shrink_spec_4_15_1158) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); "
        "  var ro := arr.AsReadOnlySpan(0, 4); "
        "  arr.RemoveAt(3); "          // Count now 3
        "  try "
        "    return ro[3]; "
        "  catch IndexOutOfRangeException => return 1; "  // caught → pass
        "  end; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// §4.15 Diagnostic: ReadOnlyArraySpan indexer setter rejected at compile time.
// ===========================================================================

// §4.15: writing through ReadOnlyArraySpan[i] must be a compile-time error.
BEZEL_TEST(array_span_549, readonly_span_setter_rejected_spec_4_15) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  var ro := a.AsReadOnlySpan(); "
        "  ro[0] := 99; "  // must be rejected — read-only span has no setter
        "  return 0; "
        "end Main; end C;"));
}

// ===========================================================================
// §4.8/§4.9 v0.155 / #920 / #1112: `.Set()` is not a method on any collection;
// undeclared methods on non-owning collections must be rejected cleanly
// (not SIGSEGV). The write surface is the indexer `coll[i] := v`.
// ===========================================================================

// §4.5 / #920 / #1112: OwningArray.Set(i,v) is rejected with an indexer hint.
BEZEL_TEST(array_span_549, ownedarray_set_rejected_with_indexer_hint_920) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); a.Add(7); "
        "  a.Set(0, 99); "  // §4.5/§4.8 v0.155: .Set is not a method — use a[0] := 99
        "  return 0; "
        "end Main; end C;"));
}

// §4.3 / #920 / #1112: Array.Set(i,v) is rejected with an indexer hint.
BEZEL_TEST(array_span_549, array_set_rejected_with_indexer_hint_920) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); "
        "  a.Set(0, 99); "  // §4.3/§4.8 v0.155: .Set is not a method — use a[0] := 99
        "  return 0; "
        "end Main; end C;"));
}

// §4.8 / #920 / #1112: Dictionary.Set(k,v) is rejected with an indexer hint.
BEZEL_TEST(array_span_549, dict_set_rejected_with_indexer_hint_920) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string,int>(); "
        "  d.Set(\"x\", 99); "  // §4.8 v0.155: .Set is not a method — use d[\"x\"] := 99
        "  return 0; "
        "end Main; end C;"));
}

// §4.8 / #920: Dictionary.Nope() is rejected with a clean diagnostic (was SIGSEGV).
BEZEL_TEST(array_span_549, dict_undeclared_method_nope_rejected_cleanly_920) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int,int>(); "
        "  d.Nope(); "  // §4.8 / #920: unknown method on Dictionary → clean error
        "  return 0; "
        "end Main; end C;"));
}

// §4.10 / #920: Set<T>.Blah() is rejected with a clean diagnostic.
BEZEL_TEST(array_span_549, set_undeclared_method_rejected_cleanly_920) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var s := new Set<int>(); "
        "  s.Blah(); "  // §4.10 / #920: unknown method on Set → clean error
        "  return 0; "
        "end Main; end C;"));
}

// Anti-over-reject guard: real methods on Array/Dictionary still accepted.
// §4.3/§4.8 / #920 / #1112.
BEZEL_TEST(array_span_549, array_and_dict_real_surface_still_accepted_920) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a[0] := 99; "
        "  using var d := new Dictionary<string,int>(); "
        "  d.Add(\"k\", 1); d[\"k\"] := 42; "
        "  var v: int? := null; d.TryGetValue(\"k\", out v); "  // v0.171 (#1336): out is V?
        "  return a.Count + a.Where(lambda(x) => x > 10).Count() + v!; "
        // a=[99,20] after a[0]:=99; both >10; 2 + 2 + 42 = 46
        "end Main; end C;", 46);
}

// Anti-over-reject guard: real methods on OwningArray still accepted.
// §4.5 / #920 / #1112.
BEZEL_TEST(array_span_549, ownedarray_real_surface_still_accepted_920) {
    expectValueAndAsanClean(
        "module M; "
        "class Cell public _v: int := 0; "
        "  public constructor(v: int) _v := v; end constructor; end Cell; "
        "class C public static method Main(): int "
        "  using var a := new OwningArray<Cell>(); "
        "  a.Add(new Cell(5)); a.Add(new Cell(7)); "
        "  a[0] := new Cell(9); "  // indexer-set disposes old Cell(5)
        "  return a.Count + a.Get(0)._v + a.Get(1)._v; "  // 2 + 9 + 7 = 18
        "end Main; end C;", 18);
}

// #1277 (§4.14): `Span<T>.Start` (field 1 of the {ptr,i32 start,i32 length}
// struct) had no member-access handler, so `sp.Start` fell through to the generic
// path and read 0 — and, in an expression, poisoned a sibling `sp.Length` read to
// 0 (`sp.Start + sp.Length` returned 0 instead of Start+Length). It now reads
// field 1 directly.
BEZEL_TEST(codegen_span549, span_start_field_read_1277) {
    // AsSpan(2, 4) → Start=2, Length=4; Start+Length = 6.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(8, lambda(i) => i + 1); "
        "  var sp := buf.AsSpan(2, 4); "
        "  return sp.Start + sp.Length; "  // 2 + 4 = 6
        "end Main; end C;"), 6);
}

// #1277: `Start` alone reads the true start (2), not 0.
BEZEL_TEST(codegen_span549, span_start_alone_1277) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(8, lambda(i) => i + 1); "
        "  var sp := buf.AsSpan(3, 2); "
        "  return sp.Start; "  // 3
        "end Main; end C;"), 3);
}

// #1277: ReadOnlySpan.Start works the same (shared {ptr,i32,i32} layout).
BEZEL_TEST(codegen_span549, readonly_span_start_field_read_1277) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(8, lambda(i) => i + 1); "
        "  var sp := buf.AsReadOnlySpan(2, 3); "
        "  return sp.Start + sp.Length; "  // 2 + 3 = 5
        "end Main; end C;"), 5);
}

// ===========================================================================
// §4.14 (#1275): direct span constructors —
//   new Span<T>(buffer, start, length) / new ReadOnlySpan<T>(...) /
//   new ArraySpan<T>(source, ...) / new ReadOnlyArraySpan<T>(...)
// are documented supported constructors (stdlib §4.14 L2179). They lower to the
// { ptr source, i32 start, i32 length } arrayspan struct. Previously they fell
// through to the generic record path (i64 Start/Length), so later i32 span reads
// picked up garbage and indexed out of bounds → SIGSEGV. Now the buffer/array
// arg is lowered to its bz_array_t* backing and the span is built directly with
// bounds checks (IndexOutOfRangeException per §4.14 L1940).
// ===========================================================================

// §4.14 (#1275): a valid direct `new Span<T>(buf, 0, 4)` reads/writes correctly
// and does not free the borrowed buffer (ASan-clean; buf disposed by `using`).
// (Plain-capacity buffer — the closure-init form leaks its env pre-existingly,
// see #1286, unrelated to the span-ctor path.)
BEZEL_TEST(codegen_span549, span_ctor_direct_valid_read_1275) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4); "
        "  var sp := new Span<int>(buf, 0, 4); "
        "  sp[3] := 4; "
        "  return sp[3]; "  // 4
        "end Main; end C;", 4);
}

// §4.14 (#1275): direct `new ReadOnlySpan<T>(buf, start, length)` over a range
// reads through the buffer written via a writable span.
BEZEL_TEST(codegen_span549, readonly_span_ctor_direct_1275) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(5); "
        "  var w := new Span<int>(buf, 0, 5); w[2] := 3; "
        "  var sp := new ReadOnlySpan<int>(buf, 1, 3); "
        "  return sp[1]; "  // buf[2] = 3
        "end Main; end C;", 3);
}

// §4.14 (#1275): direct `new ArraySpan<T>(arr, start, length)` over an Array<T>.
BEZEL_TEST(codegen_span549, arrayspan_ctor_direct_1275) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  var sp := new ArraySpan<int>(a, 1, 2); "
        "  return sp[1]; "  // arr[2] = 30
        "end Main; end C;", 30);
}

// §4.14 (#1275, #3135): length out of range against the buffer Count throws
// IndexOutOfRangeException at the FIRST OUT-OF-RANGE ACCESS, not at
// construction — stdlib §4.14 v0.195's no-eager-check rule. Before #3135 the
// throw happened at construction; after #3135 the construction succeeds and
// the indexer (#1158's `absIdx < source.Count` check) fires the same exception
// at `sp[50]`. The exception class is identical, so the program-level outcome
// (catch returns 88) is unchanged.
BEZEL_TEST(codegen_span549, span_ctor_length_oob_throws_1275) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4, lambda(i) => i + 1); "
        "  try var sp := new Span<int>(buf, 0, 100); return sp[50]; "
        "  catch IndexOutOfRangeException => return 88; end; "
        "end Main; end C;"), 88);
}

// §4.14 (#1275, #3135): start out of range throws IndexOutOfRangeException at
// the first out-of-range access. Same composition as the test above: the ctor
// succeeds (no eager check, per §4.14 v0.195), `sp[0]` checks `0 < Length (1)`
// (passes) and then `absIdx = Start + 0 = 9 ≥ source.Count = 4` (unsigned
// compare, §4.15's indexer guard) throws.
BEZEL_TEST(codegen_span549, span_ctor_start_oob_throws_1275) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4, lambda(i) => i + 1); "
        "  try var sp := new Span<int>(buf, 9, 1); return sp[0]; "
        "  catch IndexOutOfRangeException => return 77; end; "
        "end Main; end C;"), 77);
}

// §4.14 (#1286): the init lambda of `new OwningBuffer<T>(n, (i) => …)` is
// call-scoped — it fills the buffer and does not escape. Codegen now releases
// its closure cell after the fill loop; previously the closure leaked (LSan
// reported 32 bytes for a non-capturing `(i) => …`). Value: buf[3] = 4.
BEZEL_TEST(codegen_span549, ownedbuffer_init_closure_not_leaked_1286) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<int>(4, lambda(i) => i + 1); "
        "  return buf[3]; "  // 4 — and the init closure is freed (ASan-clean)
        "end Main; end C;", 4);
}
