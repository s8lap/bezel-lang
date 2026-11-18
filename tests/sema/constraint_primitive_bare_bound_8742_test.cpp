// fix #8742 (spec §14.2 *Type-parameter bounds*, v0.359): a bare
// type-parameter bound (`where S: Y`) whose substitution is a PRIMITIVE.
//
// SPEC BASIS (QUOTED, v0.359 -- spec §14.2, "Type-parameter bounds").
//   "a substitution S for `T` satisfies `where T: U` exactly when S is
//    non-nullable and is a *subtype of* the substitution for `U` -- identical
//    to it, a class derived from a class substitution (§12), or a type
//    conforming to an interface substitution (the conformance rule below) --
//    and by these three relations only: no other implicit conversion of §5.1
//    satisfies the bound, neither boxing to `Object`, numeric widening,
//    nullable wrapping nor a variance-induced conversion"
//   "At `U := Object` the bound admits `Object` and every class type, and
//    nothing else: `string`, interfaces and function types are not classes
//    (§8.1) ... at `U := long` it admits `long` alone; at a nullable
//    substitution for `U` it admits no type"
//
// BEFORE THE FIX the check read a bare bound's substitution only when it was a
// DECLARED type: a primitive has no declaration, so the check deferred and
// `D<int>.ZY<string>` was ADMITTED. Every row FAILS without the fix; the
// must-still-admit twins (`D<int>.ZY<int>`, `D<Object>.ZY<Num>`,
// `D<Animal>.ZY<Dog>`) are base-green and live in the conformance corpus.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>
#include <string_view>

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
// The shared declarations: `IBox<T>`, three conforming classes, and `D<Y>`
// with the three methods the rows call (`Z`, `ZY`, `ZS`).
constexpr const char* kPrelude = R"(
module M;
interface IBox<T>
  method Tag(): int;
end IBox;
class Num: IBox<Num>
  public constructor() end constructor;
  public method Tag(): int => 1;
end Num;
class Num2: IBox<Num2>
  public constructor() end constructor;
  public method Tag(): int => 2;
end Num2;
class Foo<U>: IBox<U>
  public constructor() end constructor;
  public method Tag(): int => 3;
end Foo;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<Y> => 1;
  public method ZY<S>(a: S): int where S: Y => 1;
  public static method ZS<S>(a: S): int where S: IBox<Y> => 1;
end D;
)";
std::string prog(std::string_view rest) {
    return std::string(kPrelude) + std::string(rest);
}
}  // namespace

// R1 -- THE ISSUE'S REPRO: `D<int>.ZY<string>` (`where S: Y`, `Y := int`).
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, primitive_class_argument_rejects_a_different_primitive) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<int>();
    return d.ZY<string>("a");
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R2 -- "at `U := long` it admits `long` alone": `D<long>.ZY<int>` (no numeric widening).
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, long_class_argument_admits_long_alone) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<long>();
    return d.ZY<int>(3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R3 -- the mirror of R2: `D<int>.ZY<long>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, int_class_argument_rejects_long) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<int>();
    return d.ZY<long>(3L);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `long` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R4 -- `D<string>.ZY<int>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, string_class_argument_rejects_int) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<string>();
    return d.ZY<int>(3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R5 -- a declared class is a subtype of no primitive: `D<int>.ZY<Num>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, primitive_class_argument_rejects_a_declared_type) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<int>();
    return d.ZY<Num>(new Num());
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R6 -- a primitive is a subtype of no declared class: `D<Num>.ZY<int>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, declared_class_argument_rejects_a_primitive) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<Num>();
    return d.ZY<int>(3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Num` (\xC2\xA7" "14.2)"));
}

// R7 -- the same defect through an INFERRED type argument (§14.5): `D<int>.ZY("a")`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, inferred_type_argument) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<int>();
    return d.ZY("a");
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R8 -- "at a nullable substitution for `U` it admits no type": `D<int?>.ZY<int>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, nullable_class_argument_admits_no_type) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<int?>();
    return d.ZY<int>(3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R9 -- "At `U := Object` the bound admits `Object` and every class type, and nothing else: `string` ... [is] not [a class]": `D<Object>.ZY<string>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, object_class_argument_does_not_admit_string) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<Object>();
    return d.ZY<string>("a");
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R10 -- a value type reaches `Object` only by boxing, which the three relations exclude: `D<Object>.ZY<int>`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, object_class_argument_does_not_admit_a_value_type) {
    JitResult r = runAot(prog(R"(
class P
  public static method Main(): int
    var d := new D<Object>();
    return d.ZY<int>(3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R11 -- the same bare bound between a method's OWN parameters: `F<string, int>` under `where A: B`.
BEZEL_TEST(sema_constraint_primitive_bare_bound_8742, sibling_method_parameter_bound) {
    JitResult r = runAot(prog(R"(
class P
  public static method F<A, B>(a: A, b: B): int where A: B => 1;
  public static method Main(): int
    return P.F<string, int>("a", 3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where A: B` (\xC2\xA7" "14.2)"));
}
