// fix #5906 / spec §14.2 *Type-parameter bounds* (v0.338): `where T: U` is
// satisfied by SUBTYPING only. A substitution S for `T` satisfies the bound
// exactly when S is non-nullable and is identical to `U`'s substitution, a class
// derived from a class substitution, or a type conforming to an interface
// substitution; no other §5.1 conversion (boxing to Object, numeric widening,
// nullable wrapping, variance) satisfies it. At `U := Object` the admitted set
// is Object and every class type (string, interfaces and function types are not
// classes, §8.1); at a nullable `U` nothing is admitted.
//
// Before the fix the instantiation-site check (Resolver::checkGenericConstraints)
// deferred with NO diagnostic whenever `U`'s substitution was not a user
// declaration, so `Rel<int, Object>`, `Rel<string, Object>`, `Rel<int, long>`,
// `Rel<IShape, Object>` and `Rel<Widget, Widget?>` were admitted silently.
//
// Every reject row asserts the §14.2 needle (never `hasErrors()` alone, which a
// crash or an unrelated error would also satisfy). The admit controls assert
// `r.ran` (never a skip-absorbing guard).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasBoundDiagnostic(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find("(§14.2)") != std::string::npos)
            return true;
    return false;
}

// Shared declarations: a generic pair with `where T: U`, and a user class and
// interface to instantiate it with.
const std::string kPrelude =
    "module M; "
    "interface IShape method Area(): int; end IShape; "
    "class Widget "
    "  public constructor() end constructor; "
    "end Widget; "
    "class Rel<T, U> where T: U "
    "  public constructor() end constructor; "
    "end Rel; ";

const std::string kMainBody =
    "class C public static method Main(): int "
    "  var r: Rel<%s>? := null; "
    "  return 0; "
    "end Main; end C;";

// Builds a program instantiating `Rel<args>` in a nullable local.
std::string program(const std::string& args) {
    std::string body = kMainBody;
    body.replace(body.find("%s"), 2, args);
    return kPrelude + body;
}

}  // namespace

BEZEL_TEST(sema_type_param_bound_subst_5906,
           primitive_arg_to_object_bound_rejected_sec_14_2) {
    // §14.2 (v0.338): boxing `int` to `Object` is not subtyping.
    auto r = runAot(program("int, Object"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `int` does not satisfy the constraint `where T: Object`"));
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           string_arg_to_object_bound_rejected_sec_14_2) {
    // §14.2 (v0.338): at `U := Object` only Object and class types are admitted;
    // `string` is not a class (§8.1).
    auto r = runAot(program("string, Object"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `string` does not satisfy the constraint `where T: Object`"));
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           interface_arg_to_object_bound_rejected_sec_14_2) {
    // §14.2 (v0.338): an interface is not a class, so it is not admitted at
    // `U := Object`.
    auto r = runAot(program("IShape, Object"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `IShape` does not satisfy the constraint `where T: Object`"));
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           numeric_widening_to_long_bound_rejected_sec_14_2) {
    // §14.2 (v0.338): numeric widening is not subtyping; at `U := long` only
    // `long` itself is admitted.
    auto r = runAot(program("int, long"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `int` does not satisfy the constraint `where T: long`"));
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           nullable_u_admits_nothing_sec_14_2) {
    // §14.2 (v0.338): at a nullable substitution for `U` nothing is admitted.
    auto r = runAot(program("Widget, Widget?"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Widget` does not satisfy the constraint `where T: Widget?`"));
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           class_arg_to_object_bound_admitted_sec_14_2) {
    // §14.2 (v0.338) control: `U := Object` admits every class type.
    auto r = runAot(program("Widget, Object"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_type_param_bound_subst_5906,
           object_arg_to_object_bound_admitted_sec_14_2) {
    // §14.2 (v0.338) control: `Object` is identical to `U`'s substitution.
    auto r = runAot(program("Object, Object"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
