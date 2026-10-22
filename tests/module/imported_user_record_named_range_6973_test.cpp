// Cross-module rows for #6973: an IMPORTED user `record Range(Lo, Hi)` is that
// record, not the builtin `Range<T>` (spec §2.8, v0.340: a type is identified
// by its declaring module; §9.8 / §12: the builtin range type).
//
// THE BUG: an imported TypeSymbol has no AST (`decl == nullptr`), and codegen
// recognized the builtin range by `declaration->name == "Range" &&
// !declaration->decl`, so the imported record was lowered as the 4-field
// `%bz.range` struct: garbage field reads, exit 70, a garbage ToString. The
// predicate is now `TypeSymbol::isBuiltinRangeSym()` (no declaring module).
//
// ORACLE: `runAotModules` (two-module AOT), exact stdout, with a same-module
// control and a builtin `1..3` range control in the importing module.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6973 =
    "module Lib6973; "
    "public record Range(Lo: int, Hi: int); "
    "public class U6973 "
    "  public static method M(): Range return new Range(3, 9); end M; "
    "  public static method Sum(r: Range): int return r.Lo + r.Hi; end Sum; "
    "end U6973;";

std::string app6973(const std::string& body) {
    return "module App6973; import Lib6973; "
           "class C6973 public static method Main(): int " + body +
           " return 0; end Main; end C6973;";
}

test::JitResult run6973(const std::string& body) {
    return test::runAotModules(
        {{"Lib6973", kLib6973}, {"App6973", app6973(body)}}, "App6973");
}

}  // namespace

// §2.8: fields, a parameter, ToString, and a local construction all see the
// record. Pre-fix: garbage ints and exit 70.
BEZEL_TEST(imported_user_record_named_range_6973,
           fields_param_tostring_and_new_spec_2_8) {
    auto r = run6973(
        "var r := U6973.M(); "
        "Console.WriteLine(r.Lo); Console.WriteLine(r.Hi); "
        "Console.WriteLine(U6973.Sum(r)); Console.WriteLine(r); "
        "var s: Range := new Range(1, 2); Console.WriteLine(s.Hi);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("3\n9\n12\nRange(Lo: 3, Hi: 9)\n2\n"));
}

// A lambda capturing the imported record (the issue's codegen failure shape).
BEZEL_TEST(imported_user_record_named_range_6973,
           lambda_capture_spec_2_8) {
    auto r = run6973(
        "var r := U6973.M(); var f := lambda() => r.Hi + r.Lo; "
        "Console.WriteLine(f());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n"));
}

// Control: a builtin range in the importing module still iterates while the
// imported `Range` record is in scope (sh2).
BEZEL_TEST(imported_user_record_named_range_6973,
           builtin_range_control_in_importer_spec_9_8) {
    auto r = run6973(
        "var r := U6973.M(); var q := 1..3; "
        "for var x in q do Console.WriteLine(x); end; "
        "Console.WriteLine(r.Hi);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n2\n3\n9\n"));
}

// Control: the same program in ONE module (a local record, which has a decl).
BEZEL_TEST(imported_user_record_named_range_6973,
           same_module_control_spec_2_8) {
    auto r = test::runAotModules(
        {{"Loc6973",
          "module Loc6973; "
          "public record Range(Lo: int, Hi: int); "
          "class C6973 "
          "  static method M(): Range return new Range(3, 9); end M; "
          "  public static method Main(): int "
          "    var r := M(); Console.WriteLine(r.Lo); Console.WriteLine(r.Hi); "
          "    Console.WriteLine(r); return 0; "
          "  end Main; end C6973;"}},
        "Loc6973");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n9\nRange(Lo: 3, Hi: 9)\n"));
}

}  // namespace bezel::module
