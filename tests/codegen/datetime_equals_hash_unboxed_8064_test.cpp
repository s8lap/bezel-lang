// Regression test for issue #8064 -- unboxed `DateTime.Equals(DateTime)` and
// `DateTime.GetHashCode()` had no lowering arm: they degraded to a zero value
// (#2434), so the program printed `False False` for two values with equal
// Ticks and different Kinds.
//
// THE SPEC. stdlib §2.5 "Equality and `Kind`" (v0.355): "A `DateTime`'s `=`,
// `<>`, `Equals` and `GetHashCode` read its `Ticks` alone, as `CompareTo`
// does: two values with equal `Ticks` are equal whatever their `Kind`s."
// spec §8.3: equal values hash equally.
//
// Every row asserts the printed value; a degraded call prints False / 0 / or
// nothing, so each expectation fails on the unfixed tree.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int "
           "    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc); "
           "    var l := DateTime.FromTicks(1000L, DateTimeKind.Local); "
           "    var d := DateTime.FromTicks(2000L, DateTimeKind.Utc); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's own repro: initializer positions.
BEZEL_TEST(datetime_equals_hash_unboxed_8064, issue_repro_initializers) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var eqm := u.Equals(l); "
        "    var hash := u.GetHashCode() = l.GetHashCode(); "
        "    Console.WriteLine($\"{eqm} {hash}\");")),
        std::string("True True"));
}

// Non-initializer positions (the statement was dropped entirely).
BEZEL_TEST(datetime_equals_hash_unboxed_8064, call_positions) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(u.Equals(l)); "
        "    Console.WriteLine(u.GetHashCode() = l.GetHashCode()); "
        "    if u.Equals(l) then Console.WriteLine(\"branch\"); end if;")),
        std::string("True\nTrue\nbranch"));
}

// Different Ticks are unequal; the same-Kind pair too.
BEZEL_TEST(datetime_equals_hash_unboxed_8064, unequal_and_same_kind) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(u.Equals(d)); "
        "    Console.WriteLine(u.Equals(u)); "
        "    Console.WriteLine(u.GetHashCode() = d.GetHashCode());")),
        std::string("False\nTrue\nFalse"));
}

// The unboxed hash agrees with the boxed one.
BEZEL_TEST(datetime_equals_hash_unboxed_8064, unboxed_agrees_with_boxed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var bo: Object := l; "
        "    Console.WriteLine(u.GetHashCode() = bo.GetHashCode()); "
        "    Console.WriteLine(u.Equals(bo));")),
        std::string("True\nTrue"));
}
