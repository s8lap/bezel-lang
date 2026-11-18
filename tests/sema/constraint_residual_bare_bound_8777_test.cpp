// fix #8777 (spec §14.2 *Type-parameter bounds*, v0.359): the residual
// silent acceptances of a METHOD's class-parameter bound `ZY<S> where S: Y`
// (`Y` the class's own type parameter) that #8741 / #8742 left deferred.
//
// SPEC BASIS (QUOTED, v0.359 -- spec §14.2, "Type-parameter bounds").
//   "a substitution S for `T` satisfies `where T: U` exactly when S is
//    non-nullable and is a *subtype of* the substitution for `U` -- identical
//    to it, a class derived from a class substitution (§12), or a type
//    conforming to an interface substitution (the conformance rule below) --
//    and by these three relations only: no other implicit conversion of §5.1
//    satisfies the bound, neither boxing to `Object`, numeric widening,
//    nullable wrapping nor a variance-induced conversion"
//   "at `U := long` it admits `long` alone; at a nullable substitution for
//    `U` it admits no type, none being identical to it and a nullable type
//    being neither a class nor an interface."
//   §9.7: a primitive, record or enum "can never" implement `IDisposable`.
//   A primitive conforms to the generic builtin interfaces only as its own
//    stdlib record declares them, each at its OWN type P alone (the F-bounded
//    numeric interfaces and the comparison pair alike).
//
// MEASURED on r30-c1 @ 29d38200e: each row's program COMPILED (rc=0). Every
// row FAILS without the fix. The must-still-admit twins (`D<IComparable<int>>
// .ZY<int>`, `D<Object>.ZY<Object>`, `D<Num>.ZY<Num>`, ...) are base-green and
// live in the conformance corpus candidates.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <string_view>

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
constexpr const char* kPrelude = R"(
module M;
interface IBox<T>
  method Tag(): int;
end IBox;
class Num: IBox<Num>
  public constructor() end constructor;
  public method Tag(): int => 1;
end Num;
class D<Y>
  public constructor()
  end constructor;
  public method ZY<S>(a: S): int where S: Y => 1;
end D;
)";

// `D<cls>.ZY<arg>(value)`: the class-parameter bound `S: Y`, `Y := cls`.
JitResult runZY(std::string_view cls, std::string_view arg,
                std::string_view value) {
    return runAot(std::string(kPrelude) + "class P\n  public static method Main(): int\n    var d := new D<" +
                  std::string(cls) + ">();\n    return d.ZY<" + std::string(arg) +
                  ">(" + std::string(value) + ");\n  end Main;\nend P;\n");
}

void expectUnsatisfied(const JitResult& r, std::string_view arg,
                       std::string_view bound) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle =
        "type argument `" + std::string(arg) +
        "` does not satisfy the constraint `where " +
        (bound == "B" ? "A" : "S") + ": " + std::string(bound) +
        "` (\xC2\xA7" "14.2)";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}
}  // namespace

// `D<IComparable<int>>.ZY<long>`: `long` is not an `IComparable<int>`.
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, interface_bound_rejects_a_different_primitive) {
    expectUnsatisfied(runZY("IComparable<int>", "long", "3L"), "long", "IComparable");
}

// `D<IComparable<long>>.ZY<int>`: no numeric widening to the conformance.
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, interface_bound_rejects_the_narrower_primitive) {
    expectUnsatisfied(runZY("IComparable<long>", "int", "3"), "int", "IComparable");
}

// `D<Num?>.ZY<Num>`: "at a nullable substitution for `U` it admits no type".
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, nullable_class_bound_admits_no_type) {
    expectUnsatisfied(runZY("Num?", "Num", "new Num()"), "Num", "Num");
}

// `D<int>.ZY<Object>`: "at `U := long` it admits `long` alone", `Object` is no `int`.
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, primitive_bound_rejects_object) {
    expectUnsatisfied(runZY("int", "Object", "new Object()"), "Object", "Y");
}

// `D<IDisposable>.ZY<int>`: a primitive can never implement `IDisposable` (§9.7).
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, disposable_bound_rejects_a_primitive) {
    expectUnsatisfied(runZY("IDisposable", "int", "3"), "int", "Y");
}

// `D<IEquatable<Object>>.ZY<int>`: `int` is an `IEquatable<int>` only.
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, equatable_object_bound_rejects_int) {
    expectUnsatisfied(runZY("IEquatable<Object>", "int", "3"), "int", "IEquatable");
}

// `D<INumber<long>>.ZY<int>`: `int` conforms to `INumber<int>` alone (stdlib §2.19).
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, static_interface_bound_rejects_a_different_primitive) {
    expectUnsatisfied(runZY("INumber<long>", "int", "3"), "int", "INumber");
}

// The method's own sibling-parameter form of the same check: `F<A, B>() where
// A: B` at `F<int, IDisposable>` (the shared code path decides both).
BEZEL_TEST(sema_constraint_residual_bare_bound_8777, sibling_parameter_bound_rejects_a_primitive_against_disposable) {
    JitResult r = runAot(R"(
module M;
class P
  public static method F<A, B>(): int where A: B => 1;
  public static method Main(): int
    return P.F<int, IDisposable>();
  end Main;
end P;
)");
    expectUnsatisfied(r, "int", "B");
}
