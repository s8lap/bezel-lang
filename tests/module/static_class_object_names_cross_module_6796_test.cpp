// #6796 (§11.7 Static classes, §12.2 Hiding is not allowed, v0.340), cross-module
// rows. §11.7: "its static members may take any name of that surface, `Equals`,
// `GetHashCode`, `ToString` and `GetType` included". A static class declared in
// another module with its own static ToString(), GetHashCode() or Equals(other:
// Object?) is admitted there, and its members are called through the qualified
// name from the importing module, from a static and from an instance caller.
// Single-module rows: tests/codegen/static_class_object_names_6796_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

const char kLib[] =
    "module Lib; "
    "public static class V "
    "  public static method ToString(): string => \"x6796\"; "
    "  public static method GetHashCode(): int => 9; "
    "  public static method Equals(other: Object?): bool => true; "
    "end V; ";

JitResult runApp(const std::string& appBody) {
    std::string app =
        "module App; import Lib; "
        "public class Program "
        + appBody +
        "end Program;";
    return runAotModules({ModuleSource{"Lib", kLib}, ModuleSource{"App", app}},
                         "App");
}

}  // namespace

// §11.7 (#6796): a static caller of an imported static class's own ToString().
BEZEL_TEST(static_class_object_names_cross_module_6796,
           imported_static_class_own_tostring_static_caller_spec_11_7) {
    JitResult r = runApp(
        "  public static method Main(): int "
        "    Console.WriteLine(Lib.V.ToString()); "
        "    return 0; "
        "  end Main; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("x6796\n"));
}

// §11.7 (#6796): a static caller of the imported static class's own GetHashCode()
// and Equals(other: Object?). The argument is an Object?-typed variable, not an
// int literal: an int literal passed to an imported Object? parameter is not boxed
// at the call site (a cross-module codegen defect, #6846, outside the hiding rule),
// so this row does not depend on it. The assertion is the same exact stdout.
BEZEL_TEST(static_class_object_names_cross_module_6796,
           imported_static_class_own_gethashcode_and_equals_static_caller_spec_11_7) {
    JitResult r = runApp(
        "  public static method Main(): int "
        "    var o: Object? := 1; "
        "    Console.WriteLine(Lib.V.GetHashCode()); "
        "    Console.WriteLine(Lib.V.Equals(o)); "
        "    return 0; "
        "  end Main; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\nTrue\n"));
}

// §11.7 (#6796): an instance caller in the importing module reaches the same member.
BEZEL_TEST(static_class_object_names_cross_module_6796,
           imported_static_class_own_gethashcode_instance_caller_spec_11_7) {
    JitResult r = runApp(
        "  public method Run(): int "
        "    Console.WriteLine(Lib.V.GetHashCode()); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var p := new Program(); "
        "    return p.Run(); "
        "  end Main; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}
