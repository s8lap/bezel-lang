// Spec-derived regression test for issue #7501 (narrow: the skipped-optional
// gap) — a named argument that SKIPS a defaulted parameter of a built-in
// constructor was rejected ("not supported on a built-in constructor").
//
// spec §22.5 "Optional parameters and named arguments" (v0.350): a named
// argument binds the parameter it names, in any order after the positional
// ones; a parameter that declares a default may stay unbound.  stdlib §2.5
// (v0.350): `TimeOnly(hour, minute, second := 0, millisecond := 0)`,
// `DateTime(year, month, day, hour, minute, second, millisecond := 0, kind)`.
//
// MEASURED on 7358f89f: `new TimeOnly(1, 2, millisecond := 5)` was refused
// with "`second` is parameter 3 but is left to its default while a later
// argument is supplied".

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(builtin_ctor_skipped_optional_7501,
           timeonly_skipping_second_binds_millisecond_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(1, 2, millisecond := 5); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 5"));
}

// Fully reordered, with `second` skipped.
BEZEL_TEST(builtin_ctor_skipped_optional_7501,
           timeonly_reordered_with_skipped_second) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(millisecond := 5, minute := 2, "
            "                          hour := 1); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 5"));
}

// All four parameters named, in an order unrelated to the declaration.
BEZEL_TEST(builtin_ctor_skipped_optional_7501,
           timeonly_all_four_named_in_any_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(millisecond := 9, second := 8, "
            "                          minute := 7, hour := 6); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("6 7 8 9"));
}
