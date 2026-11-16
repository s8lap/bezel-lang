// Spec-derived regression test for issue #8626
// (spec §14.5 phase 1 + phase 2 + phase 3, v0.357).
//
// THE DEFECT. A lambda literal whose body is a BARE `null` was admitted
// as the argument of a function-typed parameter whose return slot names
// a type parameter, and that type parameter stayed UNBOUND — the call
// compiled clean and the program computed a wrong answer.
//
// MEASURED on the pre-fix compiler (r30-c1 tip 27ad51b93):
// `Seq.M(xs, lambda(v) => null)` against `M<T, K>(xs: Array<T>, key: (T)
// -> K): T` compiled, ran, and printed the FIRST element. The lambda's
// return bound `K` to the null literal's `Object?` — the uncontexted-null
// sentinel, a binding that carries no type information — so `k` was a
// null inside the callee, `k.CompareTo(bk)` never selected anything, and
// the method returned `xs[0]`. The `where K: IComparable<K>` constraint
// was not enforced either: a nullable Object satisfies no interface
// constraint, and the constraint check has no verdict for it.
//
// WHY IT MUST BE REJECTED. §14.5 phase 1: "a bare `null` literal
// argument is not self-determined and contributes no binding". §14.5
// phase 2 binds "the lambda's inferred return type" — and a body that is
// a bare `null` has no self-determined return type. So the type
// parameter such a position was to bind stays unbound, and §14.5 phase 3
// makes that "a compile error naming the parameter".
//
// The fix is in `typeCall` (ExprTypecheck_Calls.cpp): a bare-null lambda
// body contributes no return binding (the inference), and a type
// parameter that such a position was to bind and that nothing else bound
// is reported as the phase-3 "cannot infer" error.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's own repro: the lambda's parameter is un-annotated, so the
// lambda is typed by phase 2 (§14.5) — the null body is its only return.
const char kUnannotatedLambda[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, lambda(v) => null).Name);
  end Main;
end Program;
)";

// The same shape with the lambda's parameter ANNOTATED: the lambda's
// parameter types are self-determined (§14.5 phase 1) but its RETURN is
// still the bare `null`.
const char kAnnotatedLambda[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
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
    Console.WriteLine(Seq.M(xs, lambda(v: Box) => null).Name);
  end Main;
end Program;
)";

// True iff the rejection is the §14.5 phase-3 "cannot infer" diagnostic
// naming the type parameter.
bool hasCannotInferDiag(const JitResult& r) {
    return diagnosticMessageContains(r, "cannot infer the type argument `K`") &&
           diagnosticMessageContains(r, "bare `null`");
}

}  // namespace

// ===========================================================================
// REJECT tests — these FAIL without the fix, PASS with it
// ===========================================================================

// The issue's own repro. Before the fix the call compiled, ran, and
// printed the first element (`ann`) — the type parameter `K` was bound to
// the null literal's `Object?` and the method never selected anything.
// With the fix the bare-`null` body contributes no binding, `K` remains
// unbound, and §14.5 phase 3 rejects the call naming `K`.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           unannotated_lambda_returning_null_cannot_infer_spec_14_5_phase_3) {
    auto r = runAot(kUnannotatedLambda);
    BEZEL_EXPECT_TRUE(hasCannotInferDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The annotated spelling of the same shape. The lambda's parameter type
// is self-determined (§14.5 phase 1) but its return is a bare `null`,
// which binds nothing — so `K` is still unbound and the call is still
// the phase-3 "cannot infer" error.
BEZEL_TEST(null_lambda_return_cannot_infer_8626,
           annotated_lambda_returning_null_cannot_infer_spec_14_5_phase_3) {
    auto r = runAot(kAnnotatedLambda);
    BEZEL_EXPECT_TRUE(hasCannotInferDiag(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
