// #6787, #6788, #6789 across a module boundary -- spec §35.3, §35.5, v0.340.
//
// The three §35 refusals (arity of a nested payload-case pattern, a nested head
// naming a case of another choice, a payload case used bare as a value) must
// hold when the choices are IMPORTED: the bare-value check has a separate
// imported-member arm in resolveMemberAccess. The accept rows pin the legal
// spellings of the same imported choices.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

const char kLib[] =
    "module Lib; "
    "public choice Shape Dot, Circle(R: int), Rect(W: int, H: int); end Shape; "
    "public choice Other Fetched(Status: int), Nada; end Other; "
    "public choice Holder Hold(S: Shape), Empty; end Holder; ";

JitResult runApp(const std::string& body) {
    std::string app =
        "module App; import Lib; "
        "public class Program "
        "  public static method Main(): int " + body +
        "    return 0; "
        "  end Main; "
        "end Program;";
    return runAotModules({ModuleSource{"Lib", kLib}, ModuleSource{"App", app}},
                         "App");
}

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

BEZEL_TEST(choice_case_checks_cross_module_6787_6788_6789,
           spec_35_3_imported_bare_payload_case_value_rejected) {
    JitResult r = runApp("var f := Lib.Shape.Circle; "
                         "Console.WriteLine(\"v\"); ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "payload case `Circle` cannot be used as a value"));
}

BEZEL_TEST(choice_case_checks_cross_module_6787_6788_6789,
           spec_35_5_imported_nested_head_case_of_other_choice_rejected) {
    JitResult r = runApp(
        "var h := Lib.Holder.Hold(Lib.Shape.Dot); "
        "var s := (case h of Hold(Lib.Other.Fetched(var c)) => \"f\", "
        "  _ => \"o\"); Console.WriteLine(s); ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a case of `Shape`"));
}

BEZEL_TEST(choice_case_checks_cross_module_6787_6788_6789,
           spec_35_5_imported_nested_one_slot_over_two_slot_case_rejected) {
    JitResult r = runApp(
        "var h := Lib.Holder.Hold(Lib.Shape.Rect(3, 4)); "
        "var s := (case h of Hold(Rect(_)) => \"r\", _ => \"o\"); "
        "Console.WriteLine(s); ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "binds 1 slot(s) but case `Rect` has 2 payload field(s)"));
}

BEZEL_TEST(choice_case_checks_cross_module_6787_6788_6789,
           spec_35_5_imported_legal_spellings_accepted) {
    JitResult r = runApp(
        "var h := Lib.Holder.Hold(Lib.Shape.Rect(3, 4)); "
        "var d := Lib.Shape.Dot; "
        "var s := (case h of Hold(Lib.Shape.Rect(_, _)) => \"r\", "
        "  _ => \"o\"); Console.WriteLine(s); "
        "var t := (case d of Dot => \"dot\", _ => \"o\"); "
        "Console.WriteLine(t); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("r\ndot\n"));
}
