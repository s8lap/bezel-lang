// fix #574 / §10.4 / §19.2 — enum expression-bodied property with `case this`
// body. Before the fix the case.end basic block was left without a terminator
// because enumTypeOf("this") returned null inside the inline getter evaluation
// (currentEnumMethod_ was not set), causing patternMatches to bail early.
//
// Tests:
//   1. Enum property `=> case this` selects the correct arm per value.
//   2. Wildcard (`_`) arm fires when no named arm matches.
//   3. All three enum values covered (no default needed).
//   4. Enum method `=> case this` still works (regression guard).
//   5. Non-case expression-bodied enum property still works (regression guard).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Shared enum with a `case this` property and a `case this` method.
const char* kColorEnum =
    "module M; "
    "enum Color Red, Green, Blue; "
    "  public property Code: int => "
    "    (case this of Red => 1, Green => 2, _ => 3); "
    "  public method Rank(): int "
    "    return (case this of Color.Red => 10, Color.Green => 20, Color.Blue => 30,); "
    "  end Rank; "
    "end Color; ";
} // namespace

// §10.4 / §19.2: enum expression-bodied property whose body is `case this`.
// The property returns different int values for each enum constant.
// Before fix: IR verification failure (case.end unterminated).
// After fix: correct value for each variant.
BEZEL_TEST(codegen_574_enum_prop_case, green_returns_2) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kColorEnum) +
        "class App "
        "  public static method Main(): int "
        "    var c := Color.Green; return c.Code; "
        "  end Main; "
        "end App;"), 2);
}

// §10.4 / §19.2: Red arm (first named arm).
BEZEL_TEST(codegen_574_enum_prop_case, red_returns_1) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kColorEnum) +
        "class App "
        "  public static method Main(): int "
        "    return Color.Red.Code; "
        "  end Main; "
        "end App;"), 1);
}

// §10.4 / §19.2: wildcard arm fires for Blue (not covered by named arms).
BEZEL_TEST(codegen_574_enum_prop_case, blue_wildcard_returns_3) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kColorEnum) +
        "class App "
        "  public static method Main(): int "
        "    return Color.Blue.Code; "
        "  end Main; "
        "end App;"), 3);
}

// §10.4 / §19.2: all three enum values covered by named arms (no default needed).
BEZEL_TEST(codegen_574_enum_prop_case, all_arms_explicit) {
    // Property with all three arms named — no wildcard.
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Dir North, South, East; "
        "  public property Val: int => (case this of North => 1, South => 2, East => 3,); "
        "end Dir; "
        "class App "
        "  public static method Main(): int "
        "    if Dir.North.Val = 1 and Dir.South.Val = 2 and Dir.East.Val = 3 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// §10.4 / §19.2: accessing the property via a variable (not a literal).
BEZEL_TEST(codegen_574_enum_prop_case, via_variable) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kColorEnum) +
        "class App "
        "  public static method Main(): int "
        "    var c: Color := Color.Green; "
        "    var code := c.Code; "
        "    return code; "
        "  end Main; "
        "end App;"), 2);
}

// §10.1 / §19.2: enum METHOD `=> case this` regression — must still work after
// the property fix (both share the same enumTypeOf("this") mechanism).
BEZEL_TEST(codegen_574_enum_prop_case, method_case_this_regression) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kColorEnum) +
        "class App "
        "  public static method Main(): int "
        "    if Color.Red.Rank() = 10 and Color.Green.Rank() = 20 and Color.Blue.Rank() = 30 "
        "      then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 1);
}

// §10.4: non-case expression-bodied enum property regression — a simple
// arithmetic body must still compile and return the right value.
BEZEL_TEST(codegen_574_enum_prop_case, non_case_property_regression) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Priority Low, High; "
        "  public property IsHigh: bool => false; "
        "end Priority; "
        "class App "
        "  public static method Main(): int "
        "    if not Priority.Low.IsHigh then return 7; end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 7);
}
