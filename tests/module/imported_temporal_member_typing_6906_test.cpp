// Cross-module rows for #6906: a member read on an IMPORTED temporal value
// builtin must be typed exactly as a local one (spec §2.5, v0.340: an
// imported declaration has the same meaning in the importer; stdlib §2.5
// gives `DateTime.Year` and `DateOnly.Year` the type `int`).
//
// ── THE BUG ────────────────────────────────────────────────────────────────
//
// ModuleLoader rebuilds an imported `DateTime` / `DateOnly` TypeRef as Named +
// `declaration == nullptr` + `astNode == nullptr`, with the head only in
// `importedName`. The two name helpers in sema's `resolveMemberAccess`
// (the DateTime/TimeSpan/Stopwatch/FileStream one and the
// CancellationToken/Guid/DateOnly one) read the head from the astNode only,
// so the imported receiver's member access was left UNTYPED. A bare
// `Console.WriteLine(p.Year)` still printed (codegen has its own name
// fallback), but `p.Year.ToString()` failed to compile ("`Year` on built-in
// type `DateTime` produced no value") and `var y := p.Year` gave an untyped
// local whose `ToString()` printed nothing.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules`: the two-module AOT path, the only one that produces the
// imported TypeRef. Every row asserts the exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

constexpr const char* kLib6906 =
    "module Lib6906; "
    "public class L6906 "
    "  public static method Plain(): DateTime "
    "    return new DateTime(2024, 3, 15, 4, 5, 6); "
    "  end Plain; "
    "  public static method Day(): DateOnly return new DateOnly(2023, 4, 5); end Day; "
    "end L6906;";

std::string app6906(const std::string& body) {
    return "module App6906; "
           "import Lib6906; "
           "class C6906 public static method Main(): int "
           "  " + body + " "
           "  return 0; "
           "end Main; end C6906;";
}

test::JitResult run6906(const std::string& body) {
    return test::runAotModules(
        {{"Lib6906", kLib6906}, {"App6906", app6906(body)}}, "App6906");
}

}  // namespace

// stdlib §2.5: `DateTime.Year: int`. The issue's repro: at c0a1928b the
// compile fails with "`Year` on built-in type `DateTime` produced no value".
BEZEL_TEST(imported_temporal_member_typing_6906,
           imported_datetime_year_to_string_spec_2_5) {
    auto r = run6906(
        "var p := L6906.Plain(); Console.WriteLine(p.Year.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2024\n"));
}

// stdlib §2.5: an inferred local from `p.Hour` is an `int`. At c0a1928b the
// local was untyped and the WriteLine was dropped (a #3079 warning).
BEZEL_TEST(imported_temporal_member_typing_6906,
           imported_datetime_hour_into_inferred_local_spec_2_5) {
    auto r = run6906(
        "var p := L6906.Plain(); var h := p.Hour; "
        "Console.WriteLine(h.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// stdlib §2.5: `DateOnly.Year: int`, read through the second name helper. At
// c0a1928b the WriteLine was dropped.
BEZEL_TEST(imported_temporal_member_typing_6906,
           imported_dateonly_year_into_inferred_local_spec_2_5) {
    auto r = run6906(
        "var d := L6906.Day(); var y := d.Year; "
        "Console.WriteLine(y.ToString());");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2023\n"));
}

}  // namespace bezel::module
