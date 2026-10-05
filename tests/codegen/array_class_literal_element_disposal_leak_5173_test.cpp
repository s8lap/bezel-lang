// Regression test for LEAK1: Array<T> class-element literal element-disposal leak.
//
// THE DEFECT: when an array literal contains class elements
// `const arr: Array<Rec> := [new Rec(3), new Rec(1), new Rec(2)]`
// the element disposer is not stamped in the bz_array_t header.
// When the array goes out of scope, bz_array_free is called with a null
// dispose_elem, and each class element leaks.
//
// Spec §28.9 / §28.11: a collection of owned elements must dispose its
// elements when the collection is disposed, not free the container alone.
//
// MECHANISM: materializeHeapCollectionLiteral only called
// stampOwningElemDisposer for Owning-prefixed collections.
// Array<Rec> (borrowing) was skipped, so ClassOwned elements were never
// disposed. The fix stamps the disposer for ALL collection literals with
// class/interface/Shared/Weak elements.
//
// FAIL-WITHOUT: Rec._n == 0 (3 leaks of 56 bytes each), LSan detects.
// PASS-WITH: Rec._n == 3, LSan clean.
//
// NOTE: the explicit Dispose() path (emitExplicitDisposeChain in Calls2.cpp)
// is a SEPARATE pre-existing bug — it uses the borrowing-family static
// disposer (null for class elements) rather than consulting the stamped
// header. This test covers ONLY the scope-end disposal path.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// runAotChecked is the LSan-clean oracle.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// A dispose-counting class Rec (static counter Rec._n bumped in destructor;
// instance field x carries the value). sizeof(Rec) = 56 bytes (LSan signature).
const char* kRec =
    "class Rec "
    "  public static _n: int := 0; "
    "  public x: int; "
    "  constructor (x: int) -> Self.x(x) {} "
    "  destructor Rec._n := Rec._n + 1; end destructor; "
    "end Rec; ";

std::string mainProg(const std::string& body) {
    return std::string("module M; ") + kRec +
           "class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// ===========================================================================
// PRIMARY ROW: array literal of class elements — scope-end disposal.
// Pre-fix: 3 × 56 B leaked (LSan), Rec._n == 0.
// Post-fix: LSan clean, Rec._n == 3.
// ===========================================================================
BEZEL_TEST(array_class_literal_element_disposal_leak_5173,
           literal_scope_end_disposes_elements_28_9_28_11) {
    // Loop 500× to amplify the leak signal (3 × 56 × 500 = 84,000 B).
    const std::string src = mainProg(
        "  var sum := 0; "
        "  for var i in 0..500 do "
        "    const arr: Array<Rec> := [new Rec(3), new Rec(1), new Rec(2)]; "
        "    sum := sum + arr[0].x + arr[1].x + arr[2].x; "
        "  end for; "
        "  return Rec._n; ");  // must be 1500 (3 × 500); 0 before fix (leak)
    BEZEL_EXPECT_EQ(runChecked(src), 1500);
}

// ===========================================================================
// CONTROL: element value check — program must compute correct sum even if
// disposer is missing (verifies BOTH signals independently: correct value AND
// clean sanitizer). Pre-fix: exit code 6 (sum = 6 = 3+1+2, correct), but
// LSan leak. Post-fix: exit code 3, LSan clean.
// ===========================================================================
BEZEL_TEST(array_class_literal_element_disposal_leak_5173,
           literal_value_correct_and_sanitizer_clean_28_9_28_11) {
    const std::string src = mainProg(
        "  const arr: Array<Rec> := [new Rec(3), new Rec(1), new Rec(2)]; "
        "  return Rec._n; ");  // must be 3; 0 before fix (leak)
    BEZEL_EXPECT_EQ(runChecked(src), 3);
}
