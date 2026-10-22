// Cross-module rows for #7000 (twin of #6999).
//
// Spec §2.8.3 rule 2 (v0.340): a file that imports module `A` may use values
// of any public type reachable through `A`, calling their public members.
// stdlib §9.8: `Range<T>.Contains(value: T): bool`.
//
// At HEAD the DIRECT `Contains` on an imported `Range<int>` already works
// (measured); the lambda-capture shape failed IR verification, the same
// untyped-call defect as #6999 (sema left the call untyped, lowerLambda fell
// back to an `i32` return).
//
// Oracle: `runAotModules`, exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib7000 =
    "module Lib7000; "
    "public class U7000 "
    "  public static method Mk(): Range<int> return 1..3; end Mk; "
    "  public static method RL(): Range<long> return 10L..14L step 2L; end RL; "
    "  public static method RC(): Range<char> return 'a'..'e'; end RC; "
    "end U7000;";

test::JitResult run7000(const std::string& body) {
    return test::runAotModules(
        {{"Lib7000", kLib7000},
         {"App7000",
          "module App7000; import Lib7000; "
          "class C7000 public static method Main(): int " + body +
              " return 0; end Main; end C7000;"}},
        "App7000");
}

}  // namespace

BEZEL_TEST(range_capture_contains_7000, direct_contains_control) {
    auto r = run7000("var q := U7000.Mk(); Console.WriteLine(q.Contains(2)); "
                     "Console.WriteLine(q.Contains(5));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

BEZEL_TEST(range_capture_contains_7000, lambda_captures_imported_range) {
    auto r = run7000("var q := U7000.Mk(); var f := lambda() => q.Contains(2); "
                     "Console.WriteLine(f());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

BEZEL_TEST(range_capture_contains_7000, long_and_char_and_nested) {
    auto r = run7000(
        "var l := U7000.RL(); var fl := lambda(x: long) => l.Contains(x); "
        "Console.WriteLine(fl(12L)); Console.WriteLine(fl(15L)); "
        "var c := U7000.RC(); var fc := lambda(x: char) => c.Contains(x); "
        "Console.WriteLine(fc('b')); Console.WriteLine(fc('z')); "
        "var q := U7000.Mk(); var n := lambda() => lambda() => q.Contains(3); "
        "Console.WriteLine(n()());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("True\nFalse\nTrue\nFalse\nTrue\n"));
}

}  // namespace bezel::module
