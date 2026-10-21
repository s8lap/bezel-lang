// #6763 (§11.7 Static classes, §8.2 GetType, v0.340), cross-module rows. §11.7:
// "its static members may take any name of that surface, `Equals`, `GetHashCode`,
// `ToString` and `GetType` included". A static class imported from another module
// that declares its own static `GetType` is called through its qualified name, and
// that call is the declared member, not the universal `GetType()` of #487. Single-
// module rows: tests/codegen/static_class_gettype_6763_test.cpp.

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
    "  public static method GetType(): int => 42; "
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

// §11.7 (#6763): a static caller of an imported static class's declared GetType.
BEZEL_TEST(static_class_gettype_cross_module_6763,
           imported_static_class_declared_gettype_static_caller_spec_11_7) {
    JitResult r = runApp(
        "  public static method Main(): int "
        "    Console.WriteLine(Lib.V.GetType()); "
        "    return 0; "
        "  end Main; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// §11.7 (#6763): an instance caller in the importing module reaches the same member.
BEZEL_TEST(static_class_gettype_cross_module_6763,
           imported_static_class_declared_gettype_instance_caller_spec_11_7) {
    JitResult r = runApp(
        "  public method Run(): int "
        "    Console.WriteLine(Lib.V.GetType()); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var p := new Program(); "
        "    return p.Run(); "
        "  end Main; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}
