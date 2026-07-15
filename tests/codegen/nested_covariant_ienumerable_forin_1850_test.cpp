// Codegen/iteration — iterating a NESTED covariant IEnumerable view
// (`IEnumerable<IEnumerable<T>>`) obtained from a concrete
// `Array<Array<T>>` (issue #1850, §12.9 / §29, spec v0.196).
//
// Once #1842's sema fix correctly ACCEPTS the legitimate nested covariance
// `Array<Array<Dog>>` -> `IEnumerable<IEnumerable<Dog>>` (§12.9: "position is
// composed across nested type constructors"), the program compiled but
// SEGFAULTED (SIGSEGV, null function-pointer call) the moment the nested view
// was iterated with a doubly-nested for-in.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Assign.cpp). The outer covariant view is a
// vtable-backed array enumerator (wrapArrayAsIEnumerable) whose `Current` returns
// each stored element RAW. For a plain covariant view (Array<Dog> ->
// IEnumerable<Animal>) that is correct — a Dog pointer IS an Animal pointer. But
// here each stored element is a raw `bz_array_t*` (the inner Array<Dog>), while
// the inner loop variable is typed `IEnumerable<Dog>` (§29.2 element inference =
// the outer view's genericArgs[0]) and so drives it by VTABLE dispatch
// (lowerIEnumerableBuiltinFor's pure-IEnumerable branch). Reading a bz_array
// header's first word as a vtable and calling slot 4 (GetEnumerator) jumped
// through garbage -> pc=0 SIGSEGV.
//
// FIX. When the element of an array-backed collection viewed covariantly is
// itself an array-backed collection viewed as a pure IEnumerable<T>, wrap with a
// NESTED enumerator (ensureIEnumViewFactory / wrapNestedArrayAsIEnumerable) whose
// `Current` lazily materialises a proper vtable-backed inner view per element and
// owns exactly one live inner view: MoveNext disposes the previous element's
// inner view before advancing, Dispose disposes the last one, before freeing the
// wrapper struct. The backing array is borrowed (shared), like the plain wrapper,
// so the inner Array data is still disposed exactly once by its own owner.
//
// Fails-without / passes-with: before the fix the doubly-nested for-in SIGSEGVs,
// which runAotChecked's ASan net turns into r.ran==false -> run()==-999 (never a
// legal value). After the fix the loop enumerates every element and every wrapper
// allocation is freed exactly once (no double-free / use-after-free — verified
// ASan-clean by runAotChecked).
//
// Spec: §12.9 (variance — nested covariant reference conversion, composed across
// type constructors), §29 / §29.2 (for-in desugaring & element inference), §29.11
// (GetEnumerator hands out a fresh independent enumerator; a second loop replays).
// Spec revision v0.196.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; a
// use-after-free / double-free (or the pre-fix SIGSEGV) flips r.ran to false ->
// -999, distinct from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// §12.9 / §29 (#1850): the issue's canonical repro, made leak-clean for the
// runAotChecked oracle (LSan on) by owning the Dogs in an `using var
// OwningArray<Dog>` and disposing the outer array too. The nested covariance is
// `Array<OwningArray<Dog>>` -> `IEnumerable<IEnumerable<Dog>>`; the doubly-nested
// for-in virtual-dispatches Kind() on each inner element (proving the inner
// element is a real vtable-backed object reached THROUGH the materialised inner
// view). Three Dogs, Kind()==2 each -> 6. Before the fix the inner for-in
// vtable-dispatched on a raw bz_array_t* -> SIGSEGV (-999); the OwningArray/`using
// var` only add disposal, they do not change the crashing wrap+iterate path.
BEZEL_TEST(codegen_nested_covariant_ienumerable_forin_1850,
           nested_covariant_view_iterates_dogs_spec_12_9) {
    std::string src =
        "module M; "
        "class Animal public virtual method Kind(): int return 1; end Kind; "
        "end Animal; "
        "class Dog: Animal public override method Kind(): int return 2; "
        "end Kind; end Dog; "
        "class P public static method Main(): int "
        "  using var inner: OwningArray<Dog> := "
        "      [new Dog(), new Dog(), new Dog()]; "
        "  using var outer: Array<OwningArray<Dog>> := [inner]; "
        "  var view: IEnumerable<IEnumerable<Dog>> := outer; "
        "  var total: int := 0; "
        "  for var row in view do "
        "    for var d in row do total := total + d.Kind(); end for; "
        "  end for; "
        "  return total; "                 // 3 dogs * Kind()=2 = 6
        "end Main; end P;";
    BEZEL_EXPECT_EQ(run(src), 6);
}

// §12.9 / §29 (#1850): a MULTI-element outer with primitive inner elements. Two
// inner rows [1,2,3] and [10,20] -> 6 + 30 = 36. Exercises the nested wrapper's
// per-element inner-view materialisation and its disposal on MoveNext across more
// than one row (a single-row test would never exercise the mid-loop
// dispose-previous-inner path). Before the fix: SIGSEGV (-999).
BEZEL_TEST(codegen_nested_covariant_ienumerable_forin_1850,
           nested_covariant_view_multi_row_int_elements_spec_29) {
    std::string src =
        "module M; "
        "class P public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  using var b: Array<int> := [10, 20]; "
        "  using var outer: Array<Array<int>> := [a, b]; "
        "  var view: IEnumerable<IEnumerable<int>> := outer; "
        "  var total: int := 0; "
        "  for var row in view do "
        "    for var x in row do total := total + x; end for; "
        "  end for; "
        "  return total; "                 // (1+2+3) + (10+20) = 36
        "end Main; end P;";
    BEZEL_EXPECT_EQ(run(src), 36);
}

// §29.11 (#1850): re-iterating the SAME nested view value replays from the start.
// Each outer for-in calls GetEnumerator (vtable slot 4) for a FRESH independent
// nested enumerator, so two loops over `view` each sum 1+2+3 -> 12. Pins that the
// nested wrapper's GetEnumerator clone is independent and disposed exactly once
// (a shared cursor or a double-freed clone would corrupt the heap -> -999).
BEZEL_TEST(codegen_nested_covariant_ienumerable_forin_1850,
           nested_covariant_view_replays_on_second_loop_spec_29_11) {
    std::string src =
        "module M; "
        "class P public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  using var outer: Array<Array<int>> := [a]; "
        "  var view: IEnumerable<IEnumerable<int>> := outer; "
        "  var total: int := 0; "
        "  for var row in view do "
        "    for var x in row do total := total + x; end for; "
        "  end for; "
        "  for var row in view do "
        "    for var x in row do total := total + x; end for; "
        "  end for; "
        "  return total; "                 // (1+2+3) iterated twice = 12
        "end Main; end P;";
    BEZEL_EXPECT_EQ(run(src), 12);
}

}  // namespace
