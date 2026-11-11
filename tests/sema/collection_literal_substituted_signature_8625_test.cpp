// Spec-derived regression test for issue #8625
// (spec §14.5 phase 1 + phase 2 + §22.6 + §15.4, v0.357).
//
// THE DEFECT. A collection-literal argument at a parameter whose type
// mentions a type parameter the call INFERRED was never checked against
// the SUBSTITUTED parameter type. The §22.6 applicability check runs
// "against the substituted signature" (§14.5 phase 1: "Conversions get
// their say afterward, argument by argument, at the §22.6 applicability
// check against the substituted signature"), and §15.4 types a collection
// literal against "the parameter type of a method call" — but
// `checkArgsAgainstParams` substitutes only the EXPLICIT type-argument
// list into each parameter's declared type (fix #4325's `explicitSubst`
// map), so an INFERRED type argument left the parameter type carrying a
// FREE type parameter, and every check that reads such a type DEFERS:
// `checkAssignable` skips a free-type-parameter target, and so does the
// §15.4 element check (`checkCollectionLiteralElementAssignability`).
//
// MEASURED on the pre-fix compiler (r30-c1 tip 27ad51b93):
//   `Seq.M(xs, lambda(v: Box) => v.Name + "!", [1, 2])` against
//   `M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>)` infers
//   `K := string` from the lambda's return (§14.5 phase 2), so the
//   substituted signature's third parameter is `Array<string>`; the
//   literal's `int` elements are never checked against it, codegen
//   emits the literal as `Array<int>`, and the IR fails the verifier
//   ("Call parameter type does not match function signature! i32 1 /
//   ptr call void @bz_string_retain(i32 1)" — a MALFORMED IR, the worst
//   failure mode: no diagnostic, a broken compile).
//   The CONTROL — the same call with the explicit list
//   `Seq.M<Box, string>(...)` — gets the §5.1 element rejection, twice.
//   The expected result is the control's diagnostic, not a codegen error.
//
// The fix is in `typeCall` (ExprTypecheck_Calls.cpp): after the
// argument-inferred substitution is built, the §15.4 element check runs
// against the SUBSTITUTED parameter type for the positions whose
// argument is a collection literal and whose declared parameter type
// mentions a type parameter this call inferred.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's own repro: `K` is bound by the lambda's return (§14.5
// phase 2) and the literal's `int` elements are checked against the
// substituted `Array<string>`.
const char kIntLiteralAtArrayString[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v: Box) => v.Name + "!", [1, 2]).Name);
  end Main;
end Program;
)";

// The same shape with the roles of the element type and the inferred `K`
// swapped: `K` is inferred as `int` (the lambda's return) and the
// literal's elements are `string`.
const char kStringLiteralAtArrayInt[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K, extra: Array<K>): T where K: IComparable<K>
    var best := xs[0];
    var bk := key(xs[0]);
    for var x in xs do
      var k := key(x);
      if k.CompareTo(bk) > 0 then best := x; bk := k; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M(xs, lambda(v: Box) => v.Age, ["a", "b"]).Name);
  end Main;
end Program;
)";

// True iff the rejection is the §15.4 / §5.1 element rejection the
// explicit-type-argument control produces — NOT the malformed-IR
// codegen failure the pre-fix compiler reached.
bool hasElementRejection(const JitResult& r) {
    return diagnosticMessageContains(r, "collection-literal element") &&
           diagnosticMessageContains(
               r, "not implicitly convertible to the target type (§5.1)");
}

}  // namespace

// ===========================================================================
// REJECT tests — these FAIL without the fix, PASS with it
// ===========================================================================

// The issue's own repro. Before the fix the call reached codegen, the
// literal was emitted as `Array<int>` into an `Array<string>` slot, and
// the IR failed the verifier — a malformed IR with no diagnostic. With
// the fix the literal's `int` elements are checked against the
// substituted `Array<string>` and the call is rejected at compile time
// with the §5.1 element diagnostic (the explicit-list control's).
BEZEL_TEST(collection_literal_substituted_signature_8625,
           int_literal_at_substituted_array_of_string_rejected_spec_14_5_22_6) {
    auto r = runAot(kIntLiteralAtArrayString);
    BEZEL_EXPECT_TRUE(hasElementRejection(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The reader must see the §5.1 rejection, never the malformed IR.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// The same shape with the element type and the inferred `K` swapped: the
// lambda binds `K := int` and the literal's `string` elements are
// checked against the substituted `Array<int>`.
BEZEL_TEST(collection_literal_substituted_signature_8625,
           string_literal_at_substituted_array_of_int_rejected_spec_14_5_22_6) {
    auto r = runAot(kStringLiteralAtArrayInt);
    BEZEL_EXPECT_TRUE(hasElementRejection(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}
