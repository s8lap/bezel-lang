// fix #7931 (spec §22.12 rule 2 table + note (h), v0.350): a MUTABLE static
// field of owning-inline-array type (`[N owned]T?` / `[N owned]T`) is rule 2's
// closing error ("error (iii)-closed"); the one admitted static of that type
// is `static readonly` (note (h)). Before the fix the mutable spelling was
// admitted with the retired #1393 "form (iv)" warning. Form (iv) is now the
// bare generic parameter.
//
// Rows:
//   T1  the issue's repro: `public static Pool: [2 owned]Res?;` is an error
//       at the declaration naming rule 2
//   T2  the non-nullable element form with an initializer is the same error
//   T3  an owning-inline-array auto-property stays an error (unchanged)
//   T4  ACCEPT control: the `static readonly` spelling compiles, runs, and is
//       disposed at program exit by the exit pass (note (h), §22.13)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
const char* kRes =
    "module M7931; "
    "class Res "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "  destructor Console.WriteLine($\"dispose {this.Name}\"); end destructor; "
    "end Res; ";
}  // namespace

BEZEL_TEST(sema_static_mutable_owning_array_7931,
           nullable_slot_field_is_rule_2_closing_error_note_h) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static Pool: [2 owned]Res?; "
        "end Registry; "
        "class P public static method Main() end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "mutable static field `Pool`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "does not admit as a mutable static"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "static readonly"));
}

BEZEL_TEST(sema_static_mutable_owning_array_7931,
           non_nullable_slot_field_with_initializer_is_error_note_h) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static Pool: [2 owned]Res := [new Res(\"a\"), new Res(\"b\")]; "
        "end Registry; "
        "class P public static method Main() end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "mutable static field `Pool`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "does not admit as a mutable static"));
}

BEZEL_TEST(sema_static_mutable_owning_array_7931,
           auto_property_stays_an_error) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static property Pool: [2 owned]Res?; "
        "end Registry; "
        "class P public static method Main() end Main; end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "mutable static property `Pool`"));
}

BEZEL_TEST(sema_static_mutable_owning_array_7931,
           static_readonly_owning_array_is_admitted_and_disposed_at_exit) {
    auto r = runAot(std::string(kRes) +
        "class Registry "
        "  public static readonly Pool: [2 owned]Res? := "
        "    [new Res(\"first\"), new Res(\"second\")]; "
        "end Registry; "
        "class P public static method Main() "
        "  Console.WriteLine(Registry.Pool[0]!.Name); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("first\ndispose second\ndispose first\n"));
}
