// fix #8741 (spec §14.1 *Instantiation and its site*, §14.2 *Type-parameter
// bounds* and *A type parameter as the type argument*, v0.359): a generic
// METHOD's class-parameter bound, read at a call whose receiver's class
// argument is a TYPE PARAMETER IN SCOPE, is decided by that parameter's own
// constraints -- and a concrete type is not that parameter.
//
// SPEC BASIS (QUOTED, v0.359 -- spec §14.2, "A type parameter as the type
// argument").
//   "Where a constraint's satisfaction turns on a type parameter in scope, it
//    is decided at the instantiation site (§14.1) that names the argument, by
//    the constraints of the parameters it turns on"
//   "An unsatisfied constraint is a compile error at that site, naming the
//    constraint, the argument and the parameter it turns on"
// SPEC BASIS (QUOTED, v0.359 -- spec §14.2, "Type-parameter bounds").
//   "A constraint whose bound is another type parameter -- of the same
//    declaration, or of the enclosing type -- is grammatical and has subtype
//    semantics: a substitution S for `T` satisfies `where T: U` exactly when S
//    is non-nullable and is a *subtype of* the substitution for `U`"
//
// BEFORE THE FIX, #8663 read the bound under the receiver's class argument,
// but a class argument that is a type parameter in scope (`d: D<T>`)
// substituted as a declaration-less marker, which the conformance walk reads
// as "the constraint's OWN parameter": it compared the candidate with ITSELF,
// so `d.Z<Num2>(...)` against `IBox<T>` was ADMITTED. Every row FAILS without
// the fix; the must-still-admit twins (`d.Z<T>`, the same-name collision twin,
// the `this`/derived twins) are base-green and live in the conformance corpus.

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

// R1 -- THE ISSUE'S REPRO: `P.Coll<T>(d: D<T>, ...) where T: IBox<T>`, `d.Z<Num2>(...)` needs `Num2: IBox<T>`, which no bound on `T` provides.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, issue_repro_type_parameter_receiver_argument) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<T>(d: D<T>, s: T): int where T: IBox<T>
    return d.Z<Num2>(new Num2());
  end Go;
  public static method Main(): int
    return P.Go<Num>(new D<Num>(), new Num());
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R2 -- the caller's type parameter is ALSO named `S` (the method parameter's name): the receiver's `S` is the caller's, not the constraint's.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, same_name_collision_with_the_class_parameter) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<S>(d: D<S>, s: S): int where S: IBox<S>
    return d.Z<Num2>(new Num2());
  end Go;
  public static method Main(): int
    return P.Go<Num2>(new D<Num2>(), new Num2());
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R3 -- the same defect through an INFERRED type argument (§14.5: constraints are checked after inference).
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, inferred_type_argument) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<T>(d: D<T>, s: T): int where T: IBox<T>
    return d.Z(new Num2());
  end Go;
  public static method Main(): int
    return P.Go<Num>(new D<Num>(), new Num());
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R4 -- `class G<A>: D<A>`: an INHERITED member reached through `this`, the class argument being `G`'s own parameter.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, derived_class_over_its_own_parameter) {
    JitResult r = runAot(prog(R"(
class G<A>: D<A> where A: IBox<A>
  public method Go(): int => this.Z<Num2>(new Num2());
end G;
class P
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R5 -- a call on `this` inside `Q<Y>` itself: `Y` is in scope.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, call_inside_the_generic_class_itself) {
    JitResult r = runAot(prog(R"(
class Q<Y> where Y: IBox<Y>
  public constructor() end constructor;
  public method Z<S>(a: S): int where S: IBox<Y> => 1;
  public method Go(): int => this.Z<Num2>(new Num2());
end Q;
class P
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R6 -- the same with the receiver implicit (`Z<Num2>(...)`).
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, implicit_this_call_inside_the_generic_class) {
    JitResult r = runAot(prog(R"(
class Q<Y> where Y: IBox<Y>
  public constructor() end constructor;
  public method Z<S>(a: S): int where S: IBox<Y> => 1;
  public method Go(): int => Z<Num2>(new Num2());
end Q;
class P
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R7 -- the qualified static spelling `D<T>.ZS<Num2>(...)` fixes the class instantiation (§14.1: a generic type named in receiver position of a static member access).
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, qualified_static_spelling) {
    JitResult r = runAot(prog(R"(
class H<T> where T: IBox<T>
  public method Go(): int => D<T>.ZS<Num2>(new Num2());
end H;
class P
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R8 -- a null-conditional receiver `d?.Z<Num2>(...)`.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, null_conditional_receiver) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<T>(d: D<T>?, s: T): int where T: IBox<T>
    return d?.Z<Num2>(new Num2()) ?? 0;
  end Go;
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: IBox` (\xC2\xA7" "14.2)"));
}

// R9 -- the bound IS the class parameter (`where S: Y`) and the class argument is a type parameter in scope: a concrete class is not a subtype of it.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, bare_bound_names_the_opaque_class_argument) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<T>(d: D<T>, s: T): int
    return d.ZY<Num2>(new Num2());
  end Go;
  public static method Main(): int
    return P.Go<Num>(new D<Num>(), new Num());
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R10 -- `where S: Y` at `D<T>`, `S := int`: a primitive is not a subtype of `T` either.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, bare_bound_rejects_a_primitive_argument_too) {
    JitResult r = runAot(prog(R"(
class P
  public static method Go<T>(d: D<T>, s: T): int
    return d.ZY<int>(3);
  end Go;
  public static method Main(): int
    return P.Go<int>(new D<int>(), 3);
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: Y` (\xC2\xA7" "14.2)"));
}

// R11 -- a PRIMITIVE against `IComparable<Y>` at `C<T>`: a primitive conforms only at an instantiation naming a concrete type.
BEZEL_TEST(sema_constraint_open_class_arg_bound_8741, primitive_against_builtin_interface_of_the_opaque_argument) {
    JitResult r = runAot(prog(R"(
class C<Y>
  public constructor() end constructor;
  public method Z<S>(a: S): int where S: IComparable<Y> => 1;
end C;
class P
  public static method Go<T>(c: C<T>): int
    return c.Z<int>(3);
  end Go;
  public static method Main(): int
    return 0;
  end Main;
end P;
)"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where S: IComparable` (\xC2\xA7" "14.2)"));
}
