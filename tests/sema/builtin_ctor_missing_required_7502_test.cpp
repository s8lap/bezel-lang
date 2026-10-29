// Spec-derived regression test for issue #7502 — a MISSING REQUIRED parameter
// of a built-in constructor was SILENTLY DEFAULTED instead of rejected.
//
// spec §22.5 "Optional parameters and named arguments" (v0.350): "Every
// parameter that declares no default must be bound."  spec §17.7 "The `new`
// operator": a standard-library built-in's constructor surface is exactly
// what the standard library declares for it.  stdlib §2.5 (v0.350):
//
//   DateOnly(year, month, day)
//   TimeOnly(hour, minute, second := 0, millisecond := 0)
//   DateTime(year, month, day)
//   DateTime(year, month, day, hour, minute, second, millisecond := 0, kind := …)
//
// MEASURED on 7358f89f (build/bezelc, real AOT compile + native run):
//
//   new DateOnly(2024)               -> compiled; printed 2024-01-01
//   new DateOnly()                   -> compiled; printed 0001-01-01
//   new DateTime(2024)               -> compiled; printed 2023-11-30T00:00:00
//   new DateTime(2024, 3, 4, 5)      -> compiled; printed 2024-03-04T05:00:00
//   new DateOnly(2024, 5, 6, 7)      -> compiled; the 4th argument vanished
//   new TimeOnly(5)                  -> compiled; printed 05:00:00
//
// A parameter WITH a declared default may stay unbound (`new TimeOnly(1, 2)`
// keeps second := 0); one without it may not.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
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

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dump(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// The rejection names the unsupplied REQUIRED parameter, cites §22.5, and
// the program does not compile (no object code).
void expectMissing(const std::string& body, const char* param) {
    JitResult r = runAot(prog(body));
    std::string needle = std::string("required parameter `") + param +
                         "` is not supplied";
    if (!hasError(r, needle.c_str())) dump(r);
    BEZEL_EXPECT_TRUE(hasError(r, needle.c_str()));
    BEZEL_EXPECT_TRUE(hasError(r, "§22.5"));
}

}  // namespace

// The issue's repro.
BEZEL_TEST(builtin_ctor_missing_required_7502,
           dateonly_one_argument_is_rejected_naming_month) {
    expectMissing("    var d := new DateOnly(2024); "
                  "    Console.WriteLine(d); ", "month");
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           dateonly_two_arguments_is_rejected_naming_day) {
    expectMissing("    var d := new DateOnly(2024, 5); "
                  "    Console.WriteLine(d); ", "day");
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           dateonly_no_arguments_is_rejected_naming_year) {
    expectMissing("    var d := new DateOnly(); "
                  "    Console.WriteLine(d); ", "year");
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           timeonly_one_argument_is_rejected_naming_minute) {
    expectMissing("    var t := new TimeOnly(5); "
                  "    Console.WriteLine(t); ", "minute");
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           datetime_one_argument_is_rejected_naming_month) {
    expectMissing("    var d := new DateTime(2024); "
                  "    Console.WriteLine(d); ", "month");
}

// The NAMED spelling leaves the same parameter unbound.
BEZEL_TEST(builtin_ctor_missing_required_7502,
           dateonly_named_arguments_missing_day_is_rejected) {
    expectMissing("    var d := new DateOnly(year := 2024, month := 5); "
                  "    Console.WriteLine(d); ", "day");
}

// An argument count NO declared form admits is rejected too: `DateTime`
// declares 3 or 6..8 parameters, never 4 or 5, and `DateOnly` exactly 3.
BEZEL_TEST(builtin_ctor_missing_required_7502,
           datetime_four_arguments_matches_no_declared_constructor) {
    JitResult r = runAot(prog("    var d := new DateTime(2024, 3, 4, 5); "
                              "    Console.WriteLine(d); "));
    if (!hasError(r, "matches no constructor the standard library declares"))
        dump(r);
    BEZEL_EXPECT_TRUE(
        hasError(r, "matches no constructor the standard library declares"));
    BEZEL_EXPECT_TRUE(hasError(r, "§22.5"));
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           dateonly_four_arguments_matches_no_declared_constructor) {
    JitResult r = runAot(prog("    var d := new DateOnly(2024, 5, 6, 7); "
                              "    Console.WriteLine(d); "));
    if (!hasError(r, "matches no constructor the standard library declares"))
        dump(r);
    BEZEL_EXPECT_TRUE(
        hasError(r, "matches no constructor the standard library declares"));
}

BEZEL_TEST(builtin_ctor_missing_required_7502,
           timeonly_five_arguments_matches_no_declared_constructor) {
    JitResult r = runAot(prog("    var t := new TimeOnly(5, 1, 2, 3, 4); "
                              "    Console.WriteLine(t); "));
    BEZEL_EXPECT_TRUE(
        hasError(r, "matches no constructor the standard library declares"));
}

// ---------------------------------------------------------------------------
// The admitted forms keep working, and a DECLARED DEFAULT may stay unbound.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_ctor_missing_required_7502,
           declared_forms_are_admitted_and_defaults_are_kept) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a := new DateOnly(2024, 5, 6); "
            "    var b := new TimeOnly(1, 2); "
            "    var c := new TimeOnly(hour := 1, minute := 2); "
            "    var d := new DateTime(2024, 3, 4); "
            "    var e := new DateTime(2024, 3, 4, 5, 6, 7); "
            "    var f := new DateTime(2024, 3, 4, 5, 6, 7, 8); "
            "    Console.WriteLine($\"{a} {b.Second} {b.Millisecond} "
            "{c.Minute} {d} {e.Second} {f.Millisecond}\"); ")),
        std::string("2024-05-06 0 0 2 2024-03-04T00:00:00 7 8"));
}
