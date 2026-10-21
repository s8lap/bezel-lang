// #6729 (§12.2 Hiding is not allowed, §11.7 Static classes, §8.1/§8.2, v0.340).
// §12.2: "`GetType` and `ReferenceEquals` block as every other inherited member
// does", and "a static member of the three names stays blocked (in a class that
// has instances — a `static class` is outside this rule, §11.7)". The admission
// for `Object`'s three virtual names covers their own instance signatures only,
// and `GetType` (non-virtual, §8.2) and `ReferenceEquals` (static, §8.1) have no
// `override` escape, so any inherited-name declaration in a class with instances
// is a compile error. Base accepted every reject row below (MEASURED on 97f4e965).
// §11.7 is quoted for the static-class cells: "its static members may take any
// name of that surface, `Equals`, `GetHashCode`, `ToString` and `GetType`
// included", while "`Object`'s **static** `ReferenceEquals` ... is inherited by a
// static class as by every class, and §12.2's static-member clause blocks a
// static of that name there as anywhere." The accept rows are the controls: a
// static class's static `GetType` (compile-only; a `V.GetType()` call hits the
// separate codegen limitation of #487, pinned by the lit track, not here) and the
// `Object`-slot overrides, which the rule admits.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §12.2 (v0.340, #6729): the issue's instance-`GetType` repro on a class.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           instance_gettype_on_class_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class K
  public method GetType(): int => 1;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "`GetType` is non-virtual"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): the issue's static-`ReferenceEquals` repro on a STATIC
// class. §11.7: `Object`'s static `ReferenceEquals` is inherited by a static
// class as by every class, and a static of that name is blocked there.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_referenceequals_on_static_class_rejected_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class U
  public static method ReferenceEquals(a: Object?, b: Object?): bool => true;
end U;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `ReferenceEquals` hides the inherited "
           "`Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§11.7, §12.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): a static `ReferenceEquals` on an ordinary class with
// instances. The static-class admission does not reach it.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_referenceequals_on_class_with_instances_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class K
  public static method ReferenceEquals(a: Object?, b: Object?): bool => true;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `ReferenceEquals` hides the inherited "
           "`Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "(§12.2, §8.1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): an INSTANCE `ReferenceEquals` on a class. §12.2's "block
// as every other inherited member does": no `override` reaches a static name.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           instance_referenceequals_on_class_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class K
  public method ReferenceEquals(a: Object?, b: Object?): bool => true;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `ReferenceEquals` hides the inherited "
           "`Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`ReferenceEquals` is static, so `override` is not available"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): a STATIC `GetType` on an ordinary class with instances.
// The static-class admission (§11.7) does not reach it.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_gettype_on_class_with_instances_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class K
  public static method GetType(): int => 1;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static member `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): a field named `GetType`. The diagnostic names the kind.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           field_named_gettype_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public GetType: int := 0;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "field `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): a property named `GetType`.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           property_named_gettype_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public property GetType: int => 1;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "property `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.2 (v0.340, #6729): "`GetType` is non-virtual; runtime type identity is fixed
// at allocation and cannot be overridden." An `override` of it is the conformance
// case s8_2_0002's shape, and the rejection names the non-virtual reason.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           override_gettype_rejected_non_virtual_spec_8_2) {
    auto r = runAot(R"(
module M;
public class Widget
  public override method GetType(): Type => typeof(Widget);
end Widget;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "non-virtual"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.1/§12.2 (v0.340, #6729): `override` of `ReferenceEquals`: the member is
// static, so no `override` reaches it, and the message says why.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           override_referenceequals_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class C
  public override method ReferenceEquals(a: Object?, b: Object?): bool => true;
end C;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "`ReferenceEquals` is static, so `override` is not available"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §8.2/§12.2 (v0.340, #6729): an abstract `GetType` is still a hiding: `GetType`
// has no virtual form to be abstract in.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           abstract_gettype_rejected_spec_8_2) {
    auto r = runAot(R"(
module M;
public abstract class A
  public abstract method GetType(): Type;
end A;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.7 + §12.2 (v0.340, #6729): a static FIELD named `ReferenceEquals` in a
// static class is a static member of the inherited name, and is blocked.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_field_referenceequals_on_static_class_rejected_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class S
  public static readonly ReferenceEquals: int := 1;
end S;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "field `ReferenceEquals` hides the inherited "
           "`Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.7 + §12.2 (v0.340, #6729): a static PROPERTY named `ReferenceEquals` in a
// static class is blocked the same way.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_property_referenceequals_on_static_class_rejected_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class S
  public static property ReferenceEquals: int => 1;
end S;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "property `ReferenceEquals` hides the inherited "
           "`Object.ReferenceEquals`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.3 + §12.2 (v0.340, #6729): a NESTED class named `GetType` is a member of
// the class, and it blocks the inherited `GetType` as any member does. The same
// rule already rejects a nested class named `ToString` through the Object-trio
// path, so this is the name-based rule's consequence, not a new carve-out.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           nested_class_named_gettype_rejected_spec_11_3_12_2) {
    auto r = runAot(R"(
module M;
public class K
  public class GetType
  end GetType;
end K;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "nested type `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §12.2 (v0.340, #6729): a derived class over a USER base. The base declares no
// `GetType`; the derived class's own instance `GetType` is still blocked.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           derived_class_gettype_over_user_base_rejected_spec_12_2) {
    auto r = runAot(R"(
module M;
public class B
  public Count: int := 0;
end B;
public class D: B
  public method GetType(): int => 1;
end D;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "member `GetType` hides the inherited `Object.GetType`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §11.7 (v0.340, #6729), the rule that already owns this cell: an INSTANCE
// `GetType` in a static class is "may not declare instance method" (§11.7), and
// the hiding path must not add a second, §12.2 diagnostic for the same member.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           instance_gettype_in_static_class_is_static_class_error_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class S
  public method GetType(): int => 1;
end S;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "static class `S` may not declare instance method `GetType`"));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "hides the inherited"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Accept control (§11.7, #6729): "its static members may take any name of that
// surface, `Equals`, `GetHashCode`, `ToString` and `GetType` included". A static
// class's static `GetType`. Compile-only: Main does not call it (a `V.GetType()`
// call is the separate codegen limitation, #487).
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_gettype_on_static_class_accepted_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class V
  public static method GetType(): int => 1;
end V;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ran\n");
}

// Accept control (§11.7, #6729): the same admission for a static PROPERTY named
// `GetType` in a static class.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           static_property_gettype_on_static_class_accepted_spec_11_7) {
    auto r = runAot(R"(
module M;
public static class S
  public static property GetType: int => 1;
end S;
public class Program
  public static method Main(): int
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ran\n");
}

// Accept control (§12.2 Object admission, §8.3, #6729): the three virtual names
// at their OWN signatures stay admitted as `override`s, which the change must not
// reach: `Equals(other: Object?)`, `GetHashCode()` and `ToString()`.
BEZEL_TEST(object_gettype_refeq_hiding_6729,
           object_slot_overrides_still_accepted_spec_12_2) {
    auto r = runAot(R"(
module M;
public class P
  public override method Equals(other: Object?): bool => true;
  public override method GetHashCode(): int => 1;
  public override method ToString(): string => "p";
end P;
public class Program
  public static method Main(): int
    var p := new P();
    return (if p.Equals(p) then 7 else 0);
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
