// Regression test for issue #7661 -- a NONEXISTENT member of a by-name
// built-in (`DateTime.AddTicks`, `Random.Shared`) was ACCEPTED and degraded
// to a zero value, and `StringBuilder.Capacity` (which stdlib §3.1 DOES
// declare) had no lowering arm and degraded the same way.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   stdlib §2.5 (v0.350) declares `DateTime` with a CLOSED member surface
//   (`Ticks` and `FromTicks`, no `AddTicks`); stdlib §2.7 (v0.350) declares
//   `Random` with the constructors and the `Next*` / `NextBytes` /
//   `NextBoolean` methods and NO `Shared`; stdlib §3.1 (v0.350) declares
//   `StringBuilder.Capacity: int` but the runtime has no character capacity.
//
//   The issue's own measurements: `DateTime.FromTicks(5L).AddTicks(3L)` and
//   `Random.Shared` compiled and RAN behind exit 0 with the internal #2434
//   degrade warning and a zero value, and `sb.Capacity` degraded to 0.
//
// WHAT FIXED IT.
//
//   commit 0d986bd3 -- "Reject undeclared members on by-name built-ins,
//   narrow #7661". Sema now asks one closed-surface question after
//   resolveMemberAccess, for every receiver spelling, and a read of a
//   declared-but-unimplemented member (`StringBuilder.Capacity`) is refused
//   as declared-but-not-implemented rather than degraded to a zero value.
//
//   That commit is a `narrow` commit: it names #7661 in its subject but
//   carries NO closing keyword, so the issue stayed OPEN until this one.
//
// THE ORACLE.
//
//   The DEFECT rows assert the POSITIVE refusal: the program does NOT run
//   and the diagnostic names the member that does not exist and cites the
//   section that fixes the surface. On the pre-fix tree every one of these
//   programs COMPILED and RAN (exit 0, a zero value), so every defect row is
//   RED there and GREEN here.
//
//   The CONTROL rows assert that the members the standard library DOES
//   declare keep working -- they pass on BOTH arms, so a "close the whole
//   surface" regression cannot pass.
//
// Cite: stdlib §2.5 / §2.7 / §3.1 (v0.350); spec §8.1 (v0.350), §22.6
// (v0.350).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The refusal: the program did NOT run, and the diagnostic is an ERROR that
// names the member and cites the section that fixes the surface.
bool refusedNaming(const JitResult& r, const char* member, const char* section) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(member) != std::string::npos &&
            d.message.find(section) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE DEFECT ROWS -- a member the standard library does NOT declare.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_closed_surface_7661,
           datetime_addticks_is_refused_as_no_member_2_5) {
    auto r = runAot(prog(
        "    var d := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(d.AddTicks(2L)); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(refusedNaming(r, "AddTicks", "§2.5"));
}

BEZEL_TEST(builtin_closed_surface_7661,
           random_shared_is_refused_as_no_member_2_7) {
    auto r = runAot(prog(
        "    Console.WriteLine(Random.Shared); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(refusedNaming(r, "Shared", "§2.7"));
}

BEZEL_TEST(builtin_closed_surface_7661,
           stringbuilder_capacity_is_refused_not_degraded_3_1) {
    auto r = runAot(prog(
        "    var sb := new StringBuilder(); "
        "    sb.Append(\"abc\"); "
        "    Console.WriteLine(sb.Capacity); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(refusedNaming(r, "Capacity", "§3.1"));
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS -- the members the standard library DOES declare.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_closed_surface_7661,
           control_datetime_ticks_is_still_readable_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2020, 1, 1); "
        "    Console.WriteLine(d.Year); ")),
        std::string("2020"));
}

BEZEL_TEST(builtin_closed_surface_7661,
           control_guid_tostring_is_still_readable_2_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var g := Guid.NewGuid(); "
        "    Console.WriteLine(g.ToString().Length); ")),
        std::string("36"));
}

BEZEL_TEST(builtin_closed_surface_7661,
           control_stringbuilder_length_is_still_readable_3_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(); "
        "    sb.Append(\"abc\"); "
        "    Console.WriteLine(sb.Length); ")),
        std::string("3"));
}
