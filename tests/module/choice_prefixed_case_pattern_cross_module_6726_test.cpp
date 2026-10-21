// #6726 -- spec §35.5 *Case patterns* / §19.3, v0.340 (ruling #6578): the
// prefixed choice-case pattern over an IMPORTED choice, top level and at a
// nested slot, plus the §35.5 "payload case may not be matched bare" refusal
// in its prefixed spelling. Single-module rows:
// tests/sema/choice_prefixed_case_pattern_6726_test.cpp.

#include <string>
#include <vector>

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

void expectOut(const std::string& body, const char* want) {
    JitResult r = runApp(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

bool diagContains(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_top_level_prefixed_case) {
    expectOut("var d := Lib.Shape.Dot; "
              "var s := (case d of Lib.Shape.Dot => \"dot\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "dot\n");
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_nested_prefixed_case_selects_arm) {
    expectOut("var h := Lib.Holder.Hold(Lib.Shape.Dot); "
              "var s := (case h of Hold(Lib.Shape.Dot) => \"dot\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "dot\n");
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_nested_prefixed_case_mismatch) {
    expectOut("var h := Lib.Holder.Hold(Lib.Shape.Circle(3)); "
              "var s := (case h of Hold(Lib.Shape.Dot) => \"dot\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "other\n");
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_nested_prefixed_payload_case_binds) {
    expectOut("var h := Lib.Holder.Hold(Lib.Shape.Rect(3, 4)); "
              "var s := (case h of Hold(Lib.Shape.Rect(var w, var q)) => "
              "  $\"{w}x{q}\", _ => \"other\"); Console.WriteLine(s); ",
              "3x4\n");
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_named_slot_prefixed_case) {
    expectOut("var h := Lib.Holder.Hold(Lib.Shape.Dot); "
              "var s := (case h of Hold(S = Lib.Shape.Dot) => \"dot\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "dot\n");
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_nested_prefixed_bare_payload_rejected) {
    JitResult r = runApp(
        "var h := Lib.Holder.Hold(Lib.Shape.Rect(3, 4)); "
        "var s := (case h of Hold(Lib.Shape.Rect) => \"r\", _ => \"other\"); "
        "Console.WriteLine(s); ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `Rect` requires a payload pattern"));
}

BEZEL_TEST(choice_prefixed_case_pattern_cross_module_6726,
           spec_35_5_imported_choice_top_level_prefixed_bare_payload_rejected) {
    JitResult r = runApp(
        "var d := Lib.Shape.Dot; "
        "var s := (case d of Lib.Shape.Circle => \"c\", _ => \"other\"); "
        "Console.WriteLine(s); ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `Circle` requires a payload pattern"));
}
