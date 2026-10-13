// fix #5948 / spec §14.2 *Type-parameter bounds* (v0.338): `where T: U` is
// satisfied by identity, by a class derived from the class substitution, or by
// conformance to an interface substitution. An instantiation over an ENCLOSING
// type parameter (`Rel<Box<A>, Box<A>>` inside `class H<A>`) is compared
// structurally with `A` read as the same parameter on both sides, so identical
// instantiations are admitted; the check runs at instantiation, after both
// parameters are substituted (§14.2, L2387).
//
// Accept rows assert `r.ran` and the program's stdout (a rejected program
// would not print). Reject rows assert the §14.2 needle; they never rely on
// `hasErrors()` alone.

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

// Shared declarations: a generic box, a derived box, a `where T: U` pair, and a
// user class that is not a box.
const std::string kPrelude =
    "module M; "
    "public class Box<T> public constructor() end constructor; end Box; "
    "public class Sub<T>: Box<T> public constructor() end constructor; end Sub; "
    "public class Rel<T, U> where T: U "
    "  public constructor() end constructor; "
    "end Rel; "
    "public class Widget public constructor() end constructor; end Widget; ";

}  // namespace

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           identical_open_instantiation_in_method_runs_sec_14_2) {
    // §14.2 (v0.338, L2387): `Rel<Box<A>, Box<A>>` in a method body of `H<A>`
    // is identical at every instantiation and is admitted.
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    var r: Rel<Box<A>, Box<A>>? := null; "
        "    return 7; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    var h1: H<int> := new H<int>(); "
        "    var h2: H<string> := new H<string>(); "
        "    Console.WriteLine(h1.Run().ToString()); "
        "    Console.WriteLine(h2.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n7\n"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           identical_open_instantiation_in_field_runs_sec_14_2) {
    // §14.2 (v0.338, L2387): the same identity in a field type of `H<A>`.
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  private _r: Rel<Box<A>, Box<A>>? := null; "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    return 7; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    var h1: H<int> := new H<int>(); "
        "    var h2: H<string> := new H<string>(); "
        "    Console.WriteLine(h1.Run().ToString()); "
        "    Console.WriteLine(h2.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n7\n"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           identical_open_instantiation_method_level_generic_runs_sec_14_2) {
    // §14.2 (v0.338, L2387): a method-level type parameter `A` of `M<A>`
    // names the same instantiation on both sides.
    auto r = runAot(kPrelude +
        "public class K "
        "  public static method M<A>(): int "
        "    var r: Rel<Box<A>, Box<A>>? := null; "
        "    return 3; "
        "  end M; "
        "end K; "
        "public class C "
        "  public static method Main(): int "
        "    Console.WriteLine(K.M<int>().ToString()); "
        "    Console.WriteLine(K.M<string>().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n3\n"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           derived_open_instantiation_admitted_runs_sec_14_2) {
    // §14.2 (v0.338): `Sub<A>` is a class derived from `Box<A>` over the same
    // `A`; a class derived from a class substitution satisfies the bound.
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    var r: Rel<Sub<A>, Box<A>>? := null; "
        "    return 5; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    var h: H<int> := new H<int>(); "
        "    Console.WriteLine(h.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           open_instantiation_to_object_bound_runs_sec_14_2) {
    // §14.2 (v0.338): at `U := Object` every class type is admitted, and
    // `Box<A>` is a class type whatever `A` is.
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    var r: Rel<Box<A>, Object>? := null; "
        "    return 4; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    var h: H<int> := new H<int>(); "
        "    Console.WriteLine(h.Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           concrete_mismatched_box_args_rejected_sec_14_2) {
    // §14.2 (v0.338) control, outside any generic body: `Box<string>` is not
    // identical to `Box<int>`, nor derived from it.
    auto r = runAot(kPrelude +
        "public class C "
        "  public static method Main(): int "
        "    var r: Rel<Box<int>, Box<string>>? := null; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Box` does not satisfy the constraint `where T: Box`"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           unrelated_class_arg_to_box_bound_rejected_sec_14_2) {
    // §14.2 (v0.338) control: `Widget` is neither a `Box` nor derived from one.
    auto r = runAot(kPrelude +
        "public class C "
        "  public static method Main(): int "
        "    var r: Rel<Widget, Box<int>>? := null; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Widget` does not satisfy the constraint `where T: Box`"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           nullable_box_bound_rejected_sec_14_2) {
    // §14.2 (v0.338) control: at a nullable `U` nothing is admitted.
    auto r = runAot(kPrelude +
        "public class C "
        "  public static method Main(): int "
        "    var r: Rel<Box<int>, Box<int>?>? := null; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Box` does not satisfy the constraint `where T: Box?`"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           open_arg_against_unrelated_open_bound_rejected_sec_14_2) {
    // §14.2 (v0.338): `Widget` is never a `Box<X>` at any instantiation, so the
    // constraint is unsatisfiable inside the body of `H<A>`.
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    var r: Rel<Widget, Box<A>>? := null; "
        "    return 1; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Widget` does not satisfy the constraint `where T: Box`"));
}

BEZEL_TEST(sema_type_param_bound_open_identity_5948,
           open_arg_differing_at_instantiation_rejected_sec_14_2) {
    // §14.2 (v0.338): `Rel<Box<A>, Box<int>>` holds only at `A := int`; at
    // `H<string>` the substitution is `Box<string>` against `Box<int>`, which is
    // not a subtype. The program is rejected (at the declaration today; the
    // instantiation-time deferral is not implemented, see #5948).
    auto r = runAot(kPrelude +
        "public class H<A> "
        "  public constructor() end constructor; "
        "  public method Run(): int "
        "    var r: Rel<Box<A>, Box<int>>? := null; "
        "    return 1; "
        "  end Run; "
        "end H; "
        "public class C "
        "  public static method Main(): int "
        "    var h: H<string> := new H<string>(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasBoundDiagnostic(
        r, "type argument `Box` does not satisfy the constraint `where T: Box`"));
}
