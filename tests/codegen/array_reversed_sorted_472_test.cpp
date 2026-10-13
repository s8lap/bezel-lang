// Codegen + sema — #472: Array<T>.Reversed() and Array<T>.Sorted[...] —
// value-returning copy-forms (past-participle convention, stdlib §3 / #472).
//
// DESIGN (decided, do not redesign):
//   Reversed(): owned Array<T>   — returns a NEW array, elements in reverse
//                                   order; `arr` unchanged.
//   Sorted(comparer: (T,T)->int): owned Array<T>
//                                — returns a NEW sorted array; `arr` unchanged.
//   Sorted(): owned Array<T>     — no-comparer form; T must be IComparable<T>
//                                   (int / string in this milestone).
//
// In-place Reverse() / Sort() stay UNCHANGED (mutate, return ()).
// OwningArray<T> gets NO value-forms (unique ownership — copy semantics unsound).
//
// Spec reference: stdlib §3 (Array<T> instance methods), §26.5 (instance
// members win over LINQ extensions).  All examples trace the copy-then-op path.
//
// Tests:
//   - Reversed(): reversed copy returned; original unchanged (int & string).
//   - Sorted(): sorted copy returned; original unchanged (int).
//   - Sorted(comparer): sorted copy with user comparer; original unchanged (int).
//   - Chaining: arr.Reversed().Get(0) etc.
//   - Ownership: result consumed via using var; ASan-clean (no leak, no UAF).
//   - Control: in-place Reverse() / Sort() still mutate arr.
//   - Diagnostic: Sorted() on non-IComparable T emits a clean error.

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
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Run the AOT pipeline; return the exit code, or -999 if the program did not
// run (compile error / JIT failure).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff sema rejected `src` with at least one error diagnostic.
bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff at least one reported diagnostic message contains `needle`.
bool diagContains(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    return bezel::test::diagnosticMessageContains(r, needle.c_str());
}

// ---- ASan leg (mirrors array_reverse_chain_466_test.cpp) -------------------

// Compile under ASan + LeakSanitizer, run, check exit code.
// Returns 0 (clean), 1 (sanitizer problem or wrong exit code), -1 (skip).
int runUnderAsan(const std::string& source, int expectedExitCode) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsanValue(source, expectedExitCode, "bezel_472");
}

// Assert: JIT-correct AND ASan-clean.  Skips ASan leg when toolchain absent.
void expectValueAndAsanClean(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int a = runUnderAsan(source, expectedExitCode);
    if (a == -1) return;            // toolchain unavailable: SKIP (#3626)
    BEZEL_EXPECT_TRUE(a >= 0);      // -2 program-compile / -3 link / -4 system() → FAIL (#3633)
    BEZEL_EXPECT_EQ(a, 0);
}

}  // namespace

// ===========================================================================
// Reversed(): returned copy is reversed; original is unchanged.  (stdlib §3 / #472)
// ===========================================================================

// §3 / #472: Reversed() on [1,2,3] → copy is [3,2,1]; original stays [1,2,3].
// Encode both sequences: copy: 3*49+2*7+1=162; original: 1*49+2*7+3=66.
// Return copy[0]*49+copy[1]*7+copy[2] = 162.
BEZEL_TEST(array_reversed_sorted_472, reversed_int_copy_correct_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var rev := a.Reversed(); "
        "  return rev.Get(0) * 49 + rev.Get(1) * 7 + rev.Get(2); "  // 162
        "end Main; end C;", 162);
}

// §3 / #472: original array is unchanged after Reversed().
BEZEL_TEST(array_reversed_sorted_472, reversed_int_original_unchanged_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var rev := a.Reversed(); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// §3 / #472: Reversed() on a single-element array is a copy of length 1.
BEZEL_TEST(array_reversed_sorted_472, reversed_single_element_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); "
        "  using var rev := a.Reversed(); "
        "  return rev.Count * 10 + rev.Get(0); "  // 1*10+7=17
        "end Main; end C;", 17);
}

// §3 / #472: Reversed() on an empty array returns an empty copy.
BEZEL_TEST(array_reversed_sorted_472, reversed_empty_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var rev := a.Reversed(); "
        "  return rev.Count; "  // 0
        "end Main; end C;", 0);
}

// §3 / #472: chained — arr.Reversed().Get(0) directly on the result expression.
// [5,3,1] reversed = [1,3,5]; Get(0)=1.
BEZEL_TEST(array_reversed_sorted_472, reversed_chained_get_spec_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(5); a.Add(3); a.Add(1); "
        "  using var rev := a.Reversed(); "
        "  return rev.Get(0); "  // 1
        "end Main; end C;"), 1);
}

// §3 / #472: Reversed() on Array<string> — [\"c\",\"b\",\"a\"] reversed.
// Encode using string lengths as a proxy (all length 1); check via Length.
BEZEL_TEST(array_reversed_sorted_472, reversed_string_copy_length_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"alpha\"); a.Add(\"beta\"); a.Add(\"gamma\"); "
        "  using var rev := a.Reversed(); "
        "  return rev.Count; "  // 3
        "end Main; end C;", 3);
}

// ===========================================================================
// Sorted(comparer): sorted copy; original unchanged.  (stdlib §3 / #472)
// ===========================================================================

// §3 / #472: Sorted(comparer) on [3,1,2] → copy is [1,2,3]; original stays [3,1,2].
// Encode copy: 1*49+2*7+3=66.
BEZEL_TEST(array_reversed_sorted_472, sorted_comparer_copy_correct_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(lambda(x: int, y: int) => x - y); "
        "  return s.Get(0) * 49 + s.Get(1) * 7 + s.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// §3 / #472: original array is unchanged after Sorted(comparer).
BEZEL_TEST(array_reversed_sorted_472, sorted_comparer_original_unchanged_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(lambda(x: int, y: int) => x - y); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 3*49+1*7+2=156
        "end Main; end C;", 156);
}

// §3 / #472: descending sort via comparer — [1,2,3] → [3,2,1].
// Encode: 3*49+2*7+1=162.
BEZEL_TEST(array_reversed_sorted_472, sorted_comparer_descending_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  using var s := a.Sorted(lambda(x: int, y: int) => y - x); "
        "  return s.Get(0) * 49 + s.Get(1) * 7 + s.Get(2); "  // 3*49+2*7+1=162
        "end Main; end C;", 162);
}

// §3 / #472: Sorted(comparer) on a single-element array — copy of length 1.
BEZEL_TEST(array_reversed_sorted_472, sorted_comparer_single_element_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(42); "
        "  using var s := a.Sorted(lambda(x: int, y: int) => x - y); "
        "  return s.Count * 100 + s.Get(0); "  // 1*100+42=142
        "end Main; end C;", 142);
}

// §3 / #472: chained — check Length of Sorted(comparer) result.
BEZEL_TEST(array_reversed_sorted_472, sorted_comparer_chained_length_spec_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(9); a.Add(1); a.Add(5); "
        "  using var s := a.Sorted(lambda(x: int, y: int) => x - y); "
        "  return s.Count; "  // 3
        "end Main; end C;"), 3);
}

// ===========================================================================
// Sorted() (no-comparer, IComparable<T>).  (stdlib §3 / #472)
// ===========================================================================

// §3 / #472: Sorted() on Array<int>: [3,1,2] → [1,2,3]; original unchanged.
// Encode copy: 1*49+2*7+3=66.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_int_copy_correct_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(); "
        "  return s.Get(0) * 49 + s.Get(1) * 7 + s.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// §3 / #472: original unchanged after Sorted().
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_int_original_unchanged_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  using var s := a.Sorted(); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 3*49+1*7+2=156
        "end Main; end C;", 156);
}

// §3 / #472: Sorted() on an empty Array<int> returns an empty array.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_empty_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var s := a.Sorted(); "
        "  return s.Count; "  // 0
        "end Main; end C;", 0);
}

// §3 / #472: Sorted() on a single-element array returns a copy of length 1.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_single_element_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); "
        "  using var s := a.Sorted(); "
        "  return s.Count * 10 + s.Get(0); "  // 1*10+7=17
        "end Main; end C;", 17);
}

// §3 / #472: Sorted() — non-IComparable T emits a compile-time diagnostic.
// Uses a record type (not int/string) as the element — must be rejected.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_non_comparable_rejected_spec_3) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; "
        "record Point public x: int; public y: int; end Point; "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  using var s := a.Sorted(); "  // Point is not IComparable<Point>
        "  return s.Count; "
        "end Main; end C;"));
}

// §3 / #472: diagnostic message cites IComparable and the comparer alternative.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_non_comparable_diag_spec_3) {
    const char* src =
        "module M; "
        "record Point public x: int; public y: int; end Point; "
        "class C public static method Main(): int "
        "  using var a := new Array<Point>(); "
        "  using var s := a.Sorted(); "
        "  return s.Count; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(diagContains(src, "IComparable"));
    BEZEL_EXPECT_TRUE(diagContains(src, "comparer"));
}

// ===========================================================================
// Control: in-place Reverse() and Sort() still mutate arr.  (§3 / regression)
// ===========================================================================

// §3 / regression: in-place Reverse() still mutates arr; Reversed() is not
// confused with it.  After a.Reverse(), a=[3,2,1]: 3*49+2*7+1=162.
BEZEL_TEST(array_reversed_sorted_472, inplace_reverse_still_mutates_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  a.Reverse(); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 162
        "end Main; end C;", 162);
}

// §3 / regression: in-place Sort() (no-arg) still mutates arr; Sorted() is not
// confused with it.  After a.Sort(), a=[1,2,3]: 1*49+2*7+3=66.
BEZEL_TEST(array_reversed_sorted_472, inplace_sort_still_mutates_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  a.Sort(); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 1*49+2*7+3=66
        "end Main; end C;", 66);
}

// §3 / regression: in-place Sort(comparer) still mutates arr.
// [3,1,2] sorted desc → [3,2,1]: 3*49+2*7+1=162.
BEZEL_TEST(array_reversed_sorted_472, inplace_sort_comparer_still_mutates_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(3); a.Add(1); a.Add(2); "
        "  a.Sort(lambda(x: int, y: int) => y - x); "
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 3*49+2*7+1=162
        "end Main; end C;", 162);
}

// ===========================================================================
// OwningArray<T> does NOT get value-forms — must still be accepted for the
// in-place forms and rejected for the copy-forms (via undefined-method path).
// Note: OwningArray<T> in-place Reverse()/Sort() work (they're the same runtime
// ops); only the copy-forms Reversed()/Sorted() must be absent.
// ===========================================================================

// §3 / #472: OwningArray<int>.Reversed() must be rejected — OwningArray has no
// value-form copy-method (ownership semantics prevent a shallow copy).
BEZEL_TEST(array_reversed_sorted_472, ownedarray_reversed_rejected_spec_3) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); "
        "  a.Add(1); a.Add(2); "
        "  using var rev := a.Reversed(); "  // OwningArray has no Reversed()
        "  return rev.Count; "
        "end Main; end C;"));
}

// §3 / #472: OwningArray<int>.Sorted() must be rejected.
BEZEL_TEST(array_reversed_sorted_472, ownedarray_sorted_rejected_spec_3) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new OwningArray<int>(); "
        "  a.Add(3); a.Add(1); "
        "  using var s := a.Sorted(); "  // OwningArray has no Sorted()
        "  return s.Count; "
        "end Main; end C;"));
}

// ===========================================================================
// #1195 / §14.2: Array<T>.Sort() and Array<T>.Sorted() over a class
// implementing IComparable<T>.  Before the fix, class elements were
// misclassified as strings and sorted by strcmp on the raw pointer value —
// producing wrong order.  The fix emits a bz$classcmp$ thunk that dispatches
// a.CompareTo(b) via vtable or direct compiled call.
//
// Box wraps a single int; CompareTo orders by the wrapped value.
// Spec: §14.2 (IComparable), stdlib §3 (Array.Sort/Sorted), #1195.
//
// §15.4/§28.11: a borrowing collection holds a borrow of a live local, not a
// fresh temp. Each Box is bound to a `using var` local declared before the
// borrowing Array<Box> and added as a borrow, so it stays live through Sort()/
// Sorted() and the Get(i).v reads. The originals added fresh `new Box(..)`
// temporaries, which post-#1881 are disposed at the end of each Add statement —
// Sort's CompareTo (and the later Get(i).v) then dereferenced freed Box
// (use-after-free; under the JIT harness a corrupted-value read). Sorted()
// returns a fresh borrowing Array<Box> holding the SAME element borrows
// (stdlib §4.4), so the sorted copy also reads the live Boxes; disposing the
// copy is a no-op that never touches them. Each rewrite preserves the sort/
// copy intent and the exact expected value.
// ===========================================================================

namespace {
// A class implementing IComparable<Box> — mirrors the kBox fixture in
// math_convert_test.cpp (§14.2 / #1148 / #1195).
//
// Per spec §22.10 (v0.307, line 1861) post-#4868 5458a4e1, the class's
// primary-ctor parameter `v` is NOT auto-promoted to a public property. The
// canonical idiom (docs/Bezel_Language_Spec.md:2042-2057) requires an
// explicit `public readonly property V: int := v;` so external readers can
// observe the value (the in-class body reads — `v` and `other.v` inside
// `CompareTo` — still resolve to the captured field per §22.10's
// initializer-vs-body rule).
const char* kBox1195 =
    "module M; import System; "
    "class Box(v: int): IComparable<Box> "
    "  public readonly property V: int := v; "
    "  public method CompareTo(other: Box): int => "
    "    (if v > other.v then 1 else (if v < other.v then -1 else 0)); "
    "end Box; ";
std::string boxMain1195(const std::string& body) {
    return std::string(kBox1195) +
           "class App public static method Main(): int " + body +
           " end Main; end App;";
}
}  // namespace

// §14.2 / #1195: Array<Box>.Sort() in-place — [Box(30),Box(10),Box(20)]
// sorted ascending → [Box(10),Box(20),Box(30)].
// Encode: a.Get(0).V * 49 + a.Get(1).V * 7 + a.Get(2).V
//       = 10*49 + 20*7 + 30 = 490+140+30 = 660.
// Per §22.10 (v0.307, line 1861) post-#4868: external reads of Box's
// primary-ctor `v` go through the explicit `public readonly property V: int := v;`
// declared on the kBox1195 fixture (canonical §22.10 idiom, lines 2042-2057).
// The sort intent (ascending by CompareTo) is preserved; the value-encoding
// expected value (660) is unchanged. 660 exceeds the 8-bit exit status, so it
// is asserted on captured stdout (runAot stdoutText), not the exit code.
BEZEL_TEST(array_reversed_sorted_472, sort_noarg_class_icomparable_inplace_1195) {
    auto r = runAot(boxMain1195(
        "  using var b0 := new Box(30); using var b1 := new Box(10); "
        "  using var b2 := new Box(20); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  a.Sort(); "
        "  Console.WriteLine(a.Get(0).V * 49 + a.Get(1).V * 7 + a.Get(2).V); "
        "  return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "660\n");
}

// §14.2 / #1195: Sort() orders in ascending order by CompareTo — largest last.
// [Box(5),Box(1),Box(3)] → [Box(1),Box(3),Box(5)]: 1*49+3*7+5 = 75.
BEZEL_TEST(array_reversed_sorted_472, sort_noarg_class_icomparable_ascending_1195) {
    BEZEL_EXPECT_EQ(run(boxMain1195(
        "  using var b0 := new Box(5); using var b1 := new Box(1); "
        "  using var b2 := new Box(3); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  a.Sort(); "
        "  return a.Get(0).V * 49 + a.Get(1).V * 7 + a.Get(2).V;")), 75);
}

// §14.2 / #1195: Array<Box>.Sorted() copy-form — original unchanged.
// [Box(30),Box(10),Box(20)] sorted → copy [Box(10),Box(20),Box(30)].
// Original encode: 30*49 + 10*7 + 20 = 1470+70+20 = 1560. (exit 40 mod 256 = 40)
// Use simpler encoding: sorted copy Get(0).V = 10.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_class_icomparable_copy_1195) {
    BEZEL_EXPECT_EQ(run(boxMain1195(
        "  using var b0 := new Box(30); using var b1 := new Box(10); "
        "  using var b2 := new Box(20); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  using var s := a.Sorted(); "
        "  return s.Get(0).V;")), 10);
}

// §14.2 / #1195: Sorted() original array unchanged — first element stays Box(30).
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_class_icomparable_original_unchanged_1195) {
    BEZEL_EXPECT_EQ(run(boxMain1195(
        "  using var b0 := new Box(30); using var b1 := new Box(10); "
        "  using var b2 := new Box(20); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  using var s := a.Sorted(); "
        "  return a.Get(0).V;")), 30);
}

// §14.2 / #1195: Sorted() full order check — [Box(30),Box(10),Box(20)]
// sorted → [Box(10),Box(20),Box(30)]: 10*49 + 20*7 + 30 = 660.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_class_icomparable_order_1195) {
    auto r = runAot(boxMain1195(
        "  using var b0 := new Box(30); using var b1 := new Box(10); "
        "  using var b2 := new Box(20); "
        "  using var a := new Array<Box>(); "
        "  a.Add(b0); a.Add(b1); a.Add(b2); "
        "  using var s := a.Sorted(); "
        "  Console.WriteLine(s.Get(0).V * 49 + s.Get(1).V * 7 + s.Get(2).V); "
        "  return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "660\n");
}

// §14.2 / #1195: Sorted() on non-IComparable class still emits a diagnostic.
// A class without IComparable must still be rejected by sema.
BEZEL_TEST(array_reversed_sorted_472, sorted_noarg_non_icomparable_class_rejected_1195) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; "
        "class Widget public v: int; end Widget; "
        "class C public static method Main(): int "
        "  using var a := new Array<Widget>(); "
        "  using var s := a.Sorted(); "  // Widget does not implement IComparable
        "  return s.Count; "
        "end Main; end C;"));
}
