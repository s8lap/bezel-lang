// narrow #8796 (spec §14.2 *Type-parameter bounds*, stdlib §9.6, v0.359): a
// primitive against a declaration-less built-in iteration interface bound, at
// a TYPE-INSTANTIATION site. (The call-site arms live in
// ExprTypecheck_Calls.cpp.)
//
// SPEC BASIS (QUOTED, v0.359). §14.2: "a type satisfies an interface bound
// only by conforming to that interface at that instantiation". stdlib §9.6:
// "public class String: IEnumerable<char>, IComparable<string>,
// IEquatable<string>" -- a string is a sequence of `char`.
//
// FAILS without the fix (MEASURED on the base: both programs COMPILE, rc=0).
// Must-still-admit twins (`string` against `IEnumerable<char>`) are corpus
// candidates.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;


// #8796 (type-instantiation site): `string` is an `IEnumerable<char>`, not an
// `IEnumerable<int>`.
BEZEL_TEST(sema_bound_conformance_8796, string_is_no_ienumerable_of_int_at_an_instantiation) {
    JitResult r = runAot(R"(
module M;
class Box<S> where S: IEnumerable<int>
  public constructor() end constructor;
  public method N(): int => 1;
end Box;
class P
  public static method Main(): int
    var b := new Box<string>();
    return b.N();
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where S: IEnumerable` (\xC2\xA7" "14.2)"));
}

// #8796: no primitive other than `string` is an `IEnumerable<...>` at all.
BEZEL_TEST(sema_bound_conformance_8796, int_is_no_ienumerable_at_an_instantiation) {
    JitResult r = runAot(R"(
module M;
class Box<S> where S: IEnumerable<int>
  public constructor() end constructor;
  public method N(): int => 1;
end Box;
class P
  public static method Main(): int
    var b := new Box<int>();
    return b.N();
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: IEnumerable` (\xC2\xA7" "14.2)"));
}
