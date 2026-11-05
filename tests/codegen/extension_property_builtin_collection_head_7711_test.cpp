// Pin test for issue #7711 -- an extension PROPERTY on a built-in collection
// head resolves. spec §26.3 / §26.5 step 2 (v0.355): extension members include
// computed read-only properties, and are found when the receiver's type
// declares no instance member of that name.
//
// ALREADY FIXED on staging b52409a7 (verified by lane r39-verify-vB); this
// file only PINS it (no fail-without is available).
//
// The `Count` row: `Array<T>` DECLARES an instance `Count`, so §26.5 step 1
// ("Instance members win -- by name") commits instance resolution and the
// extension property is never consulted: the program prints the real length
// 3, not the extension's 77. Measured, then pinned as the spec-stated result.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
std::string prog(const std::string& props, const std::string& body) {
    return "module R;\n"
           "extension Array<int> " + props + " end;\n"
           "class Program public static method Main(): int\n"
           "  var a: Array<int> := [1,2,3];\n" + body +
           "  return 0;\n"
           "end Main; end Program;\n";
}
}  // namespace

BEZEL_TEST(extension_property_builtin_collection_head_7711,
           total_property_on_array_head_spec_26_5_v0_355) {
    auto r = runAotChecked(prog("public property Total: int => 77;",
                                "  Console.WriteLine(a.Total);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("77\n"));
}

BEZEL_TEST(extension_property_builtin_collection_head_7711,
           two_properties_on_array_head_spec_26_5_v0_355) {
    auto r = runAotChecked(
        prog("public property Total: int => 77; "
             "public property Count2: int => 5;",
             "  Console.WriteLine(a.Total); Console.WriteLine(a.Count2);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("77\n5\n"));
}

BEZEL_TEST(extension_property_builtin_collection_head_7711,
           instance_count_wins_over_extension_property_spec_26_5_step_1) {
    auto r = runAotChecked(prog("public property Count: int => 77;",
                                "  Console.WriteLine(a.Count);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}
