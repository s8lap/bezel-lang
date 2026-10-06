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
//
// Two spellings here were never valid Bezel and are now written the way
// Appendix A.4's `ConstructorDecl` and §13.3 require:
//   * §13.3 — every class member declares its visibility explicitly; only
//     `static constructor`, `destructor`, `polymorphic` and `invariant`
//     members carry none. An instance `constructor` therefore needs
//     `public` (spec line 12174, Appendix C.5's member-visibility rule).
//   * Appendix A.4 — `ConstructorDecl ::= Visibility "pure"? "constructor"
//     "(" ParamList? ")" ContractClause* CtorChain? Statement* "end"
//     "constructor"? ";"`. There is no initializer-list alternative and no
//     `Self.` spelling anywhere in the grammar, so `constructor (x: int)
//     -> Self.x(x) {}` has no production; the field is assigned in the
//     body, which is what `CtorChain`-then-`Statement*` admits.
const char* kRec =
    "class Rec "
    "  public static _n: int := 0; "
    "  public x: int; "
    "  public constructor (x: int) this.x := x; end constructor; "
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
// Pre-fix: 3 × 56 B leaked per iteration (LSan), Rec._n == 0.
// Post-fix: LSan clean, Rec._n == 3 per iteration.
// ===========================================================================
//
// The loop counter `0..500` is a CLOSED range (§16.3 — `..` includes both
// endpoints), so it runs 501 times, not 500: 501 × 3 = 1503 disposals, not
// 1500. The OS exit-status mechanism is 8 bits, so §33.3's "the returned
// value becomes the process exit code" delivers 1503 truncated to 223, and a
// row asserting an exit code could not distinguish the right program from a
// wrong one that happens to truncate to the same byte. The observable
// therefore travels on captured stdout (the repo's standing pattern for an
// observable above 255, e.g. dict_string_key_leak_3703), and the assertion is
// on the exact string.
BEZEL_TEST(array_class_literal_element_disposal_leak_5173,
           literal_scope_end_disposes_elements_28_9_28_11) {
    // Loop 501× to amplify the leak signal (3 × 56 × 501 = 84,168 B).
    const std::string src = mainProg(
        "  var sum := 0; "
        "  for var i in 0..500 do "
        "    const arr: Array<Rec> := [new Rec(3), new Rec(1), new Rec(2)]; "
        "    sum := sum + arr[0].x + arr[1].x + arr[2].x; "
        "  end for; "
        "  Console.WriteLine($\"{Rec._n}\"); "  // 501 iterations × 3 = 1503
        "  return 0; ");                       // 0 before fix (leak), 1503 after
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);   // ASan/LSan-clean: nothing leaked
    BEZEL_EXPECT_EQ(r.stdoutText, "1503\n");
}

// ===========================================================================
// CONTROL: element disposal count — the literal's three class elements are
// disposed EXACTLY once each, so Rec._n reads 3 after the literal's scope
// ends. Pre-fix: 0 (the header carried no element disposer, so bz_array_free
// freed the container alone) plus an LSan report of 3 × 56 B. Post-fix: 3,
// LSan clean.
//
// The literal sits in an inner `do … end` block on purpose. Its scope-end
// disposal is the thing under test, and a `return` expression is evaluated
// BEFORE the enclosing frame's scope-end disposals run — so a literal
// declared directly in Main's body would still read Rec._n == 0 at the
// return, observing the counter before the disposal it is counting. The
// inner block ends first, so the read sees the post-disposal value.
// ===========================================================================
BEZEL_TEST(array_class_literal_element_disposal_leak_5173,
           literal_value_correct_and_sanitizer_clean_28_9_28_11) {
    const std::string src = mainProg(
        "  do "
        "    const arr: Array<Rec> := [new Rec(3), new Rec(1), new Rec(2)]; "
        "  end; "
        "  return Rec._n; ");  // must be 3; 0 before fix (leak)
    BEZEL_EXPECT_EQ(runChecked(src), 3);
}
