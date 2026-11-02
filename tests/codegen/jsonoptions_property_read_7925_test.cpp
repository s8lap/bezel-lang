// Regression for #7925 -- `Console.WriteLine(o.IndentWidth)` on a
// `new JsonOptions(...)` local failed to lower ("the argument of this
// `Console.WriteLine` failed to lower to a value", #3079) and printed nothing.
//
// per stdlib §2.15 (v0.350): `JsonOptions` is a positional record
// `JsonOptions(Indented: bool, IndentWidth: int, AllowTrailingCommas: bool,
// IgnoreUnknownFields: bool, MaxDepth: int)`; "`new JsonOptions(...)` supplies
// every one" of the five components, and a record component is readable as a
// property. No read of any of the five was lowered.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kPre =
    "module M; import System; import System.Serialization; "
    "class P public static method Main() ";
}  // namespace

// All five component reads, with values that differ per component so a
// misordered field index shows.
BEZEL_TEST(jsonoptions_property_read_7925, all_five_components_print) {
    auto r = runAotChecked(std::string(kPre) +
        "var o := new JsonOptions(true, 4, false, true, 9); "
        "Console.WriteLine(o.Indented); Console.WriteLine(o.IndentWidth); "
        "Console.WriteLine(o.AllowTrailingCommas); "
        "Console.WriteLine(o.IgnoreUnknownFields); Console.WriteLine(o.MaxDepth); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n4\nFalse\nTrue\n9\n"));
}

// The mirrored booleans (so a constant-true read cannot pass), the issue's
// narrow `byte` width argument, and reads used as operands rather than printed.
BEZEL_TEST(jsonoptions_property_read_7925, mirrored_bools_and_arithmetic) {
    auto r = runAotChecked(std::string(kPre) +
        "var w: byte := 200; var o := new JsonOptions(false, w, true, false, 7); "
        "Console.WriteLine(o.Indented); Console.WriteLine(o.AllowTrailingCommas); "
        "Console.WriteLine(o.IgnoreUnknownFields); "
        "var d: int := o.IndentWidth + o.MaxDepth; Console.WriteLine(d); "
        "if o.AllowTrailingCommas and not o.Indented then Console.WriteLine(\"ok\"); end if; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nTrue\nFalse\n207\nok\n"));
}

// A read through a `const` local and through a parameter of the record type.
BEZEL_TEST(jsonoptions_property_read_7925, const_local_and_parameter) {
    auto r = runAotChecked(
        "module M; import System; import System.Serialization; "
        "class P public static method Width(o: JsonOptions): int => o.IndentWidth; "
        "public static method Main() "
        "const o := new JsonOptions(true, 3, true, true, 5); "
        "Console.WriteLine(o.MaxDepth); Console.WriteLine(Width(o)); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n3\n"));
}
