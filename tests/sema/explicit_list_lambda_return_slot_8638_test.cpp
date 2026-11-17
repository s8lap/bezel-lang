// fix #8638 (§22.6 *Lambda-literal arguments*, §14.5 *Explicit lists are
// all-or-nothing*, v0.357). "When a full explicit list is supplied, no
// inference runs": the listed types are the bindings, so at
// `Seq.M<Box, string>(xs, key: (T) -> K)` the lambda's return slot is the
// CLOSED `string`. §22.6: "the candidate is applicable at the position when the
// lambda's inferred return type equals the candidate's return slot, implicitly
// converts to it (§5.1), or binds it". A bare `null` body converts to no
// non-nullable `string` and binds nothing, so the call is rejected (the
// non-generic twin `N(xs, f: (Box) -> string)` already was).
//
// Before the fix the pre-resolution lambda visit (ExprTC::visitChildren,
// `formalFnParamType2`) gave up on every bare-own-generic position under an
// explicit list, so the lambda was typed against no formal and its return was
// never checked: the call compiled and printed the first element, `ann`. The
// substitution of a full list landed with narrow #8394; these rows pin the
// issue's own repro and its un-annotated twin.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
const char kAnnotatedNullBody[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M<Box, string>(xs, lambda(v: Box) => null).Name);
  end Main;
end Program;
)";

const char kUnannotatedNullBody[] = R"(
module P;
public class Box
  public Name: string;
  public Age: int;
  public constructor(n: string, a: int) Name := n; Age := a; end constructor;
end Box;
public static class Seq
  public static method M<T, K>(xs: Array<T>, key: (T) -> K): T where K: IComparable<K>
    var best := xs[0];
    for var x in xs do
      if key(x).CompareTo(key(best)) > 0 then best := x; end;
    end;
    return best;
  end M;
end Seq;
public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("cho", 40), new Box("bob", 25), new Box("dan", 35)];
    Console.WriteLine(Seq.M<Box, string>(xs, lambda(v) => null).Name);
  end Main;
end Program;
)";

void expectNullBodyRejected(const char* src) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the lambda body's returned value is nullable but the target type "
           "is non-nullable"));
}
}  // namespace

// The issue's repro: `Seq.M<Box, string>(xs, lambda(v: Box) => null)`.
BEZEL_TEST(sema_explicit_list_lambda_return_slot_8638,
           spec_22_6_explicit_list_annotated_null_body_rejected) {
    expectNullBodyRejected(kAnnotatedNullBody);
}

// The same call with the lambda parameter un-annotated (`lambda(v) => null`).
BEZEL_TEST(sema_explicit_list_lambda_return_slot_8638,
           spec_22_6_explicit_list_unannotated_null_body_rejected) {
    expectNullBodyRejected(kUnannotatedNullBody);
}
