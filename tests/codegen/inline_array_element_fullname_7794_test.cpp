// Regression for #7794 -- the default `ToString` of an inline array rendered its
// element by the bare declared name (`"[2]Rec"`). Spec §8.5 (v0.355): an inline
// array's name is its stdlib §2.13 `FullName` rendering, the element qualified
// (rule 4, the element recursing in that form): `"[2]M.Rec"`, and an owning
// `[2 owned]Res` renders `"[2 owned]M.Res"`. Keyword-named elements are
// unchanged (`"[3]int"`, `"[2]string"`).
//
// The single rendering site is `inlineArrayTypeSpelling`, which all three
// renderers (direct call, interpolation, Console.WriteLine) and the box thunk
// route through, so each row checks all three call forms against one spelling.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kRec =
    "module M; "
    "record Rec(x: int); "
    "class Res public constructor() end constructor; end Res; "
    "class P public static method Main() ";
}  // namespace

// Record element: all three call forms agree on the qualified element.
BEZEL_TEST(inline_array_element_fullname_7794,
           record_element_qualified_in_all_three_forms_spec_8_5_v0_355) {
    auto r = runAotChecked(std::string(kRec) +
        "var a: [2]Rec := [new Rec(1), new Rec(2)]; "
        "Console.WriteLine(a.ToString()); "
        "Console.WriteLine($\"{a}\"); "
        "Console.WriteLine(a); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(
        "[2]M.Rec\n[2]M.Rec\n[2]M.Rec\n"));
}

// Owning class element: the `owned` marker and the qualified element both show.
BEZEL_TEST(inline_array_element_fullname_7794,
           owning_class_element_qualified_spec_8_5_v0_355) {
    auto r = runAot(std::string(kRec) +
        "var own: [2 owned]Res := [new Res(), new Res()]; "
        "Console.WriteLine(own.ToString()); "
        "Console.WriteLine($\"{own}\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(
        "[2 owned]M.Res\n[2 owned]M.Res\n"));
}

// Generic element: the instantiation renders its head and arguments (rule 3).
BEZEL_TEST(inline_array_element_fullname_7794,
           generic_element_qualified_with_arguments_spec_8_5_v0_355) {
    auto r = runAotChecked(
        "module M; "
        "class Box<T> public constructor() end constructor; end Box; "
        "class P public static method Main() "
        "using var b1 := new Box<int>(); using var b2 := new Box<int>(); "
        "var a: [2]Box<int> := [b1, b2]; "
        "Console.WriteLine(a.ToString()); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[2]M.Box<int>\n"));
}

// Enum element and interface element: both are FullName-qualified.
BEZEL_TEST(inline_array_element_fullname_7794,
           enum_and_interface_element_qualified_spec_8_5_v0_355) {
    auto r = runAotChecked(
        "module M; "
        "enum Col Red, Green; end Col; "
        "interface I method GetV(): int; end I; "
        "class W: I public constructor() end constructor; "
        "  public method GetV(): int => 1; end W; "
        "class P public static method Main() "
        "var e: [2]Col := [Col.Red, Col.Green]; "
        "Console.WriteLine(e.ToString()); "
        "using var w1 := new W(); using var w2 := new W(); "
        "var i: [2]I := [w1, w2]; "
        "Console.WriteLine(i.ToString()); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[2]M.Col\n[2]M.I\n"));
}

// CONTROL: keyword-named elements are unchanged by the qualification.
BEZEL_TEST(inline_array_element_fullname_7794,
           keyword_element_unchanged_control_spec_8_5_v0_355) {
    auto r = runAotChecked(
        "module M; "
        "class P public static method Main() "
        "var a: [3]int := [1, 2, 3]; "
        "var s: [2]string := [\"x\", \"y\"]; "
        "Console.WriteLine(a.ToString()); "
        "Console.WriteLine($\"{s}\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[3]int\n[2]string\n"));
}
