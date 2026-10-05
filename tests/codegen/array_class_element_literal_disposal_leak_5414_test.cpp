// Regression test for #5414: `Array<T>` of class elements never wires
// dispose_elem — every element leaks when the array goes out of scope.
//
// THE DEFECT: `materializeHeapCollectionLiteral` calls
// `stampOwningElemDisposer(arr, elemRef, isOwningPrefixedName(name))`
// but `isOwningPrefixedName("Array")` is false, so the call is a no-op.
// For class-element arrays (Array<Rec> where Rec is a class), the elements
// ARE owned by the array (§28.11: class/interface × borrowing → owned slot)
// and the array's scope-end free MUST dispose each element via the vtable.
// Today it does not: `bz_array_free(arr, 0)` with dispose_elem = null drops
// the elements.
//
// MECHANISM: `materializeHeapCollectionLiteral` at LlvmBackend_Setup.cpp
// ~40992 stamps element disposers only for owning collections
// (OwningArray/OwningStack/OwningQueue). For a non-owning Array<Rec>
// literal, `stampOwningElemDisposer(arr, elemRef, false)` returns early
// without installing the vtable disposer. The fix adds a path that stamps
// the class-element disposer (elementDisposerArg(elemRef, owningFamily=true))
// for non-owning Array/Stack/Queue of class elements.
//
// Spec: stdlib §4.4 (Array<T> owns its elements), §28.9 / §28.11 (disposal).
// The string-element path (`litIsStrArr`) is already correct — strings are
// RuntimeManaged and need no deep disposal; the class-element path was missing.
//
// CONTROLS:
//   (a) string-element array still works and does not leak (baseline)
//   (b) OwningArray<Rec> literal (using var) still works correctly
//
// FAIL-WITHOUT: 200 × [new Rec(), new Rec(), new Rec()] → 200 × 3 = 600
// leaked Rec instances, ~33,600 B at 56 B each. LSan fails before value assertion.
// PASS-WITH: LSan clean; R._n == 0 (every Rec disposed exactly once).
//
// LOOP AMPLIFICATION: per [[lsan-one-shot-leak-tests-pass-vacuously]] — a
// one-shot leak that might be reclaimed by an unrelated path cannot pass
// 200–500 iterations. 200 iterations × 3 elements = 600 leaked objects is
// well past LSan noise floor.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked is the LSan-clean oracle. A leak (the defect) fails the row
// before the exit value is examined. The explicit `R._n == 0` assertion
// in the program itself is the reader-trustworthy oracle — it cannot be
// faked by a skipped leg and distinguishes "disposed" from "leaked" exactly.
int checkedExit(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Rec: a class with a static disposal counter. R._n is net-live count:
// +1 on ctor, -1 on dtor. The program asserts R._n == 0 at the end.
// If elements leak, R._n > 0 and the assertion fails (exit != 0).
// If elements are double-freed, R._n < 0 and the assertion fails.
const std::string kCounterRec =
    "class R "
    "  public static _n: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; R._n := R._n + 1; end constructor; "
    "  destructor R._n := R._n - 1; end destructor; "
    "end R; ";

}  // namespace

// ===========================================================================
// PRIMARY ROW: Array<Rec> literal, 200 iterations, scope-end disposal.
// Pre-fix: 600 Rec instances leak (200 × 3).
// Post-fix: R._n == 0, LSan clean, exit 0.
// ===========================================================================
BEZEL_TEST(array_class_element_literal_disposal_leak_5414,
           literal_scope_end_disposes_class_elements_4_4_28_9) {
    // Return R._n directly: 0 means all elements disposed, >0 means leak.
    // Using return value avoids Console.WriteLine and keeps exit code clean.
    // NOTE: while loop (not for...in) because 'var' keyword changes semantics.
    // NOTE: 'const arr' (not 'var arr') — 'var' triggers the §15.4 fresh-producer
    // reject for a non-owning Array of class elements; 'const' is accepted.
    const std::string src =
        "module M; " + kCounterRec +
        "class C  public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    const arr: Array<R> := [new R(3), new R(1), new R(2)]; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(R._n); "
        "  return R._n; "
        "  end Main; end C; ";
    BEZEL_EXPECT_EQ(checkedExit(src), 0);
}

// ===========================================================================
// CONTROL (a): string-element array still works and does not leak.
// Strings are RuntimeManaged (refcounted); no deep disposal needed.
// Baseline: this was already correct before the fix.
// ===========================================================================
BEZEL_TEST(array_class_element_literal_disposal_leak_5414,
           control_string_element_array_no_leak_4_4) {
    const std::string src =
        "module M; "
        "class C  public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    var arr: Array<string> := [i.ToString(), (i + 1).ToString()]; "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "  end Main; end C; ";
    // runAot (no LSan needed; string elements don't need deep disposal)
    auto r = runAot(src);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// ===========================================================================
// CONTROL (b): OwningArray<Rec> literal (using var) still works correctly.
// The owning collection path (stampOwningElemDisposer with owning=true) was
// already correct; this is a regression control to ensure the fix to the
// non-owning path does not break the owning path.
// ===========================================================================
BEZEL_TEST(array_class_element_literal_disposal_leak_5414,
           control_owning_array_rec_literal_still_works_4_4) {
    const std::string src =
        "module M; " + kCounterRec +
        "class C  public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var arr: OwningArray<R> := [new R(3), new R(1), new R(2)]; "
        "    i := i + 1; "
        "  end; "
        "  return R._n; "
        "  end Main; end C; ";
    BEZEL_EXPECT_EQ(checkedExit(src), 0);
}
