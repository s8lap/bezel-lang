// Codegen/iteration — a lazy LINQ pipeline (`Select`/`Where`/… → a `bz_lazy_t*`)
// bound to an EXPLICIT `IEnumerable<T>`-typed local and then enumerated
// (issue #1911, stdlib §5 / §29.11).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp lowerLocalDecl). A lazy LINQ
// operator lowers to a runtime `bz_lazy_t*` whose vtable has only FOUR slots
// ([0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current) and whose Current returns a
// POINTER to the element bytes. When such a value is bound to a local declared as
// the pure interface `IEnumerable<T>` (interface widening), the for-in over that
// local drives the §29.11 iterator-vtable protocol
// (lowerIEnumerableBuiltinFor's pure-IEnumerable branch): it reads vtable slot 4
// (GetEnumerator) — OUT OF BOUNDS for the 4-slot lazy vtable, so the loaded
// garbage word is called as a function -> pc≈0 SIGSEGV — and expects Current BY
// VALUE (the lazy Current returns a pointer). lowerLocalDecl wrapped an Array /
// LinkedList / user-class initializer into a proper vtable-backed enumerator but
// had NO case for a lazy pipeline, so the raw `bz_lazy_t*` was stored unwrapped.
// The identical program with the annotation REMOVED (an inferred local) kept its
// concrete lazy type and iterated via lowerLazyLinqFor (the 4-slot lazy protocol,
// Current-by-pointer) — so it worked; only the interface-widened binding crashed.
//
// FIX. lowerLocalDecl now recognises the lazy sentinel initializer
// (isLazyEnumerableType) bound to a pure `IEnumerable<T>` local and drains the
// pipeline once into a fresh OWNED Array<T> (bz_lazy_to_array — which also
// disposes the pipeline, releasing its inline-lambda closures), then wraps that
// array in an OWNING vtable-backed enumerator (wrapOwnedArrayAsIEnumerable). That
// wrapper carries the full 5-slot §29.11 vtable (Current-by-value, GetEnumerator
// at slot 4 handing out NON-owning clones), so the for-in dispatch is in-bounds
// and correct, and scope-end iterEnumDispose (registerIterableSourceDisposal /
// registerUsingDisposal) frees the drained array exactly once — leak-free, no
// double-free (each per-loop clone borrows the shared array and frees only its
// own struct).
//
// Fails-without / passes-with: before the fix the for-in SIGSEGVs, which
// runAotChecked's ASan net turns into r.ran==false -> run()==-999 (never a legal
// value). After the fix the loop enumerates every element and every allocation is
// freed exactly once (verified ASan/LSan-clean by runAotChecked). The element
// type is `int` throughout so the drained snapshot owns no heap elements (the
// pre-existing lazy string-minting-selector leak, independent of #1911, is out of
// scope here).
//
// Spec: stdlib §5 (deferred LINQ operators), §29 / §29.2 (for-in desugaring &
// element inference), §29.11 (vtable-backed IEnumerable<T>: slot-1 Dispose /
// slot-4 GetEnumerator). Spec revision v0.196.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; the pre-fix
// SIGSEGV (or any leak / double-free) flips r.ran to false -> -999, distinct from
// any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// stdlib §5 / §29.11 (#1911): the issue's canonical repro — a plain-lambda
// `Select` (independent of method references) bound to an explicit
// `IEnumerable<int>` local, then enumerated with for-in. Two strings "ab","cde"
// -> lengths 2,3 -> sum 5. Before the fix: SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_local_forin_1911,
           select_lambda_bound_to_ienumerable_local_forin_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var strings: Array<string> := [\"ab\", \"cde\"]; "
        "  var lens: IEnumerable<int> := strings.Select(lambda(s) => s.Length); "
        "  var sum: int := 0; "
        "  for var x in lens do sum := sum + x; end for; "
        "  return sum; "                     // 2 + 3 = 5
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// stdlib §5 (#1911): the same widening bug through a lazy `Where` (a different
// lazy operator, exercising the same lowerLocalDecl bridge). Of {1,2,3,4} keep
// n>2 -> {3,4} -> sum 7. Before the fix: SIGSEGV (-999).
BEZEL_TEST(codegen_lazy_linq_ienumerable_local_forin_1911,
           where_lambda_bound_to_ienumerable_local_forin_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var nums: Array<int> := [1, 2, 3, 4]; "
        "  var big: IEnumerable<int> := nums.Where(lambda(n) => n > 2); "
        "  var sum: int := 0; "
        "  for var x in big do sum := sum + x; end for; "
        "  return sum; "                     // 3 + 4 = 7
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// stdlib §5 (#1911): a CHAINED lazy pipeline (`Where(...).Select(...)`) bound to a
// `using var` explicit `IEnumerable<int>` — the using-var disposal path
// (registerUsingDisposal) as opposed to the plain-var path
// (registerIterableSourceDisposal). Of {10,20,30} keep n>15 -> {20,30}, /5 ->
// {4,6} -> sum 10. Before the fix: SIGSEGV (-999). ASan-clean confirms the owning
// wrapper's scope-end Dispose frees the drained array exactly once.
BEZEL_TEST(codegen_lazy_linq_ienumerable_local_forin_1911,
           chained_pipeline_using_var_ienumerable_local_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var nums: Array<int> := [10, 20, 30]; "
        "  using var v: IEnumerable<int> := "
        "      nums.Where(lambda(n) => n > 15).Select(lambda(n) => n / 5); "
        "  var sum: int := 0; "
        "  for var x in v do sum := sum + x; end for; "
        "  return sum; "                     // 20/5 + 30/5 = 4 + 6 = 10
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 10);
}

// §29.11 (#1911): binding materialises the lazy pipeline into a re-iterable
// snapshot, so a SECOND for-in over the same `IEnumerable<int>` local replays
// (GetEnumerator slot 4 hands out a fresh NON-owning clone each loop). Two loops
// over lengths {2,3} -> 5 + 5 = 10. Pins that the owning wrapper's clone is
// independent and disposed exactly once (a shared cursor or a double-freed
// clone/array would corrupt the heap -> -999). Before the fix the first loop
// already SIGSEGV'd.
BEZEL_TEST(codegen_lazy_linq_ienumerable_local_forin_1911,
           second_forin_over_ienumerable_local_replays_spec_29_11) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var strings: Array<string> := [\"ab\", \"cde\"]; "
        "  var lens: IEnumerable<int> := strings.Select(lambda(s) => s.Length); "
        "  var sum: int := 0; "
        "  for var x in lens do sum := sum + x; end for; "
        "  for var y in lens do sum := sum + y; end for; "
        "  return sum; "                     // (2+3) + (2+3) = 10
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 10);
}

}  // namespace
