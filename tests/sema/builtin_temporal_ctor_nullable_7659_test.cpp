// Regression test for issue #7659 (spec §7, §7.3, §7.4, §22.6, stdlib §2.5,
// v0.350).
//
// THE DEFECT. "built-in DateTime/TimeSpan/DateOnly constructors accept an
// `int?` argument (nonNullable mask 0x00), then abort the IR verifier".
//
// stdlib §2.5 (v0.350) declares the temporal records' constructors with
// INTEGER components:
//
//   public constructor(year: int, month: int, day: int);
//   public constructor(year: int, month: int, day: int, hour: int, …);
//
// and §7.3 is explicit: "Assigning `null` to a non-nullable target is a
// compile error." The `kBuiltinParams` rows of the ONE built-in parameter
// table carried `nonNullable` 0x00 on the temporal constructors, so the §7
// nullability gate named NO position of them and the nullable reached
// codegen -- where it aborted the IR verifier.
//
// MEASURED on the tree before the fix (the parent of the commit that set the
// masks, b1dcf5eb):
//
//   `new DateTime(i, 1, 1)` with `i: int?`  -> accepted, then
//                                                 "codegen error: IR
//                                                 verification failed"
//   the same with a §7.4-NARROWED `i`         -> the same abort
//
// WHAT FIXED IT. Two earlier commits, neither of which named #7659 in a
// closing position:
//
//   * b1dcf5eb ("Extract narrowed nullables at built-in constructors and
//     record parameters, fix #7692") set the temporal constructors' §7.3
//     `nonNullable` masks (0x07 / 0x07 / 0x0F) and published the narrowed
//     payload at those positions (the §7.4 extraction twin of the member
//     gate, fix #7621); and
//   * this lane's own commit 2b9d116a ("fix #8014: the `new TimeSpan(...)`
//     argument positions carry the intrinsic's type") gave the `TimeSpan`
//     constructor's positions their type and their §7 nullability --
//     stdlib §2.5 declares NO `TimeSpan` constructor at all, so the ONE table
//     carries no row for it and the `TimeSpan` half of this issue needed the
//     constructor's own check.
//
// `scripts/pick_issues.sh --github b1dcf5eb` prints `#7692` and
// `scripts/pick_issues.sh --github 2b9d116a` prints `#8014`, so #7659 was
// never closed even though its defect was already gone from the tree. This
// file pins it.
//
// THE ORACLE. `runAot` is the strict AOT oracle (fork-isolated
// Driver::compileFile + clang link + native exec). Every REFUSAL row asserts
// the POSITIVE: the exact §7 diagnostic text, at a REAL source location
// (failedAtCompileTimeWithRealLocation). Every ADMISSION row asserts the
// exact stdout the standard library's constructor computes. No row asserts
// the absence of a diagnostic.
//
// ASan is OFF in lanes by default (CLAUDE.md): this is an argument's
// nullability admission and a diagnostic, not allocation, layout,
// retain/release or disposal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

}  // namespace

namespace {

// A program whose class body is `body` verbatim.
std::string progClass(const std::string& body) {
    return "module M;\npublic class C\n" + body + "end C;\n";
}

// A program whose `Main` body is `body`.
std::string prog(const std::string& body) {
    return progClass("  public static method Main(): int\n" + body +
                     "    return 0;\n"
                     "  end Main;\n");
}

// A refusal row: the program must NOT run, the diagnostic must carry `needle`
// at a REAL source location.
void expectRefused(const std::string& body, const char* needle) {
    JitResult r = runAot(body);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
    // The diagnostic is LOCATED: the driver's captured stderr carries a
    // `<path>:<line>:<col>:` prefix, so the refusal has a real source
    // position rather than a synthesized one.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile CLEAN, run, exit 0 and print exactly
// this stdout.
void expectStdout(const std::string& body, const std::string& expected) {
    JitResult r = runAot(body);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

// The UN-NARROWED shape: the nullable reaches the constructor through a
// PARAMETER, so the callee cannot know it is not null (§7.3). A local
// initialised with a non-null literal would be NARROWED by the compiler and
// would not exercise the gate at all.
std::string viaParameter(const std::string& ctor) {
    return progClass(
        "  public static method T(i: int?): int\n"
        "    var t := new " + ctor + "(i, 1, 1);\n"
        "    Console.WriteLine(t);\n"
        "    return 0;\n"
        "  end T;\n"
        "  public static method Main(): int\n"
        "    var x: int? := 2020;\n"
        "    Console.WriteLine(T(x));\n"
        "    return 0;\n"
        "  end Main;\n");
}

}  // namespace

// ===========================================================================
// #7659 -- the §7 REFUSALS. One row per temporal constructor.
// ===========================================================================

// stdlib §2.5 declares `DateTime(year: int, month: int, day: int)`, so the
// first component is a NON-nullable `int` (§7.3) and a nullable there is a
// compile error -- not a verifier abort.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_nullable_at_the_date_time_year_is_refused_7) {
    expectRefused(viaParameter("DateTime"),
                  "the argument to `DateTime` constructor is nullable but the "
                  "target type is non-nullable (§7)");
}

// The same for `DateOnly(year: int, month: int, day: int)`.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_nullable_at_the_date_only_year_is_refused_7) {
    expectRefused(viaParameter("DateOnly"),
                  "the argument to `DateOnly` constructor is nullable but the "
                  "target type is non-nullable (§7)");
}

// The same for `TimeOnly(hour: int, minute: int, …)`.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_nullable_at_the_time_only_hour_is_refused_7) {
    expectRefused(viaParameter("TimeOnly"),
                  "the argument to `TimeOnly` constructor is nullable but the "
                  "target type is non-nullable (§7)");
}

// The `TimeSpan` constructor's positions -- the compiler's own intrinsic,
// which stdlib §2.5 does not declare. Its nullable is refused at the
// position it binds.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_nullable_at_the_time_span_minutes_is_refused_7) {
    expectRefused(
        progClass("  public static method T(i: int?): int\n"
                  "    var t := new TimeSpan(1, i, 3);\n"
                  "    Console.WriteLine(t);\n"
                  "    return 0;\n"
                  "  end T;\n"
                  "  public static method Main(): int\n"
                  "    var x: int? := 2;\n"
                  "    Console.WriteLine(T(x));\n"
                  "    return 0;\n"
                  "  end Main;\n"),
        "the `minutes` argument of the `TimeSpan` constructor is nullable but "
        "the target type is non-nullable (§7)");
}

// ===========================================================================
// #7659 -- the ADMISSION side. A §7.4-NARROWED read is of its non-nullable
// type (§22.6), so it is ADMITTED at a non-nullable position -- and the
// payload is EXTRACTED, so the program runs and computes the value.
// ===========================================================================

// A narrowed `DateTime` year.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_narrowed_nullable_at_the_date_time_year_is_admitted_22_6) {
    expectStdout(prog("    var i: int? := 2020;\n"
                      "    if i is not null then\n"
                      "      var t := new DateTime(i, 1, 1);\n"
                      "      Console.WriteLine(t);\n"
                      "    end if;\n"),
                 "2020-01-01T00:00:00");
}

// A narrowed `DateOnly` year.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_narrowed_nullable_at_the_date_only_year_is_admitted_22_6) {
    expectStdout(prog("    var i: int? := 2020;\n"
                      "    if i is not null then\n"
                      "      var t := new DateOnly(i, 1, 1);\n"
                      "      Console.WriteLine(t);\n"
                      "    end if;\n"),
                 "2020-01-01");
}

// A narrowed `TimeOnly` hour.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_narrowed_nullable_at_the_time_only_hour_is_admitted_22_6) {
    expectStdout(prog("    var i: int? := 1;\n"
                      "    if i is not null then\n"
                      "      var t := new TimeOnly(i, 1, 1);\n"
                      "      Console.WriteLine(t);\n"
                      "    end if;\n"),
                 "01:01:01");
}

// A narrowed `TimeSpan` minute.
BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           a_narrowed_nullable_at_the_time_span_minutes_is_admitted_22_6) {
    expectStdout(prog("    var i: int? := 2;\n"
                      "    if i is not null then\n"
                      "      var t := new TimeSpan(1, i, 3);\n"
                      "      Console.WriteLine(t);\n"
                      "    end if;\n"),
                 "01:02:03");
}

// ===========================================================================
// THE CONTROL ROW -- the conforming construction still computes stdlib §2.5's
// value. This is what proves the gate refuses the NULLABLE argument and not
// the constructor.
// ===========================================================================

BEZEL_TEST(builtin_temporal_ctor_nullable_7659,
           the_conforming_date_time_construction_still_computes_2_5) {
    expectStdout(prog("    var t := new DateTime(2020, 1, 1);\n"
                      "    Console.WriteLine(t);\n"),
                 "2020-01-01T00:00:00");
}
