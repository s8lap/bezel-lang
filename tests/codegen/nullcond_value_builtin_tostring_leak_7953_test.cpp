// Codegen — §7.5 / §28.9 / stdlib §2.5 / §2.6 / #7953: the `?.` spelling of a
// BUILT-IN VALUE type's `ToString()` is a +1 string PRODUCER, so its reference
// is discharged once — the `?.` spelling leaked it.
//
// THE DEFECT (#7953, MEASURED on the parent of this fix). `g?.ToString()` on a
// `Guid?` lowers through `lowerNullCondValueSemCall` to the SAME
// `emitValueToString` the `.` spelling uses, so the two produce the SAME fresh
// `rc := 1` heap `bz.string` (a bare `null` on the null path, which every
// release tolerates). The producer recognizer
// (`Emitter::isPlusOneStringProducer`, src/codegen/LlvmBackend_Setup.cpp)
// already had ONE `.`-spelling arm per built-in value type:
//
//   #1982  Guid.ToString()                          -> bz_guid_to_string
//   #1988  DateTime/TimeSpan/DateOnly/TimeOnly       -> bz_*_to_string_*
//   #1989  Guid.ToString(format)                    -> bz_guid_to_string_fmt
//   #1637  StringBuilder.ToString()                 -> bz_strbuilder_to_string
//
// but the `?.` arm read only the PRIMITIVE receivers (#6680). So the `?.`
// spelling fell through to the BORROW reading: the bind RETAINED the string
// (rc 1 -> 2) and the scope-end release dropped the borrow (rc 2 -> 1), never
// dropping the producer's original +1 — the 32-byte header + the data buffer
// leaked per call.
//
// MEASURED (ASan/LSan) on the parent of this fix:
//   * `var g: Guid? := Guid.Empty; var s: string? := g?.ToString();` in a
//     200-iteration loop -> "Direct leak of 6400 byte(s) in 200 object(s)" plus
//     "Indirect leak of 7400 byte(s) in 200 object(s)" (13 800 B in 400
//     allocations);
//   * the byte-identical `.` spelling (`var s: string? := g.ToString();`) is
//     CLEAN on the same tree — so the defect is the `?.` SPELLING, not the
//     receiver;
//   * the `?.` FORMAT spelling (`g?.ToString("N")`) leaks the same way
//     (1600 B in 50 objects over a 50-iteration loop);
//   * the interpolation hole `$"{g?.ToString()}"` leaks the same way.
//
// THE FIX. The `?.` arm of the producer recognizer now reads the SAME
// receiver-type test the `.` arms read (the built-in head name), so the two
// decisions cannot drift: a receiver the `?.` arm admits is exactly a receiver
// the `.` arm already calls a +1 producer. The ARITY follows the `.` arms too:
// no argument for every head in the family, one argument (the format
// specifier) only for the three heads that declare a `ToString(format)`
// overload (Guid / DateTime / TimeSpan).
//
// §7.5 ("Null-conditional member access and invocation"): `x?.M` resolves as
// the plain member access `x.M` on a non-null `x`, and yields `null` only for
// a null receiver. The `?.` spelling therefore produces exactly what the `.`
// spelling produces — including the §28.9 obligation to discharge the string
// producer's reference.
//
// §28.9 (runtime-managed values / the producer's reference): a fresh `string`
// minted at `rc := 1` is owned by exactly one reference; the consumer
// discharges it once. Reading it as a BORROW (retain at bind, release at scope
// end) leaves the producer's reference behind — a leak, measured above.
//
// stdlib §2.5 `DateTime`, `TimeSpan`, `DateOnly`, `TimeOnly` and §2.6 `Guid`
// ("`ToString()`: string" — the canonical / ISO-8601 / `c` renderings): the
// rows below assert the RENDERED TEXT, so a fix that only silenced the leak
// (and rendered the wrong value) would fail.
//
// ORACLE DISCIPLINE. A LEAK IS ASSERTED BY THE SANITIZER, NOT BY A COUNT
// INVENTED HERE: every row below runs under the harness's ASan/LeakSanitizer
// configuration (`runAotChecked`) and asserts the run is CLEAN (`ran` true,
// the program's own exit code). The exit code carries the observed VALUE, so
// each row asserts a NUMBER (or an exact stdout) and never a negative the
// harness cannot observe.
//
// ⚠ THE `?.` NULL PATH. The null path of `?.` yields `null`, and every
// release tolerates a null pointer; the rows below therefore exercise a
// NON-NULL receiver only, so the row's value is the rendered text. The
// null-receiver behaviour is §7.5's own subject and is not this fix's.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;

namespace {

// (1) THE ISSUE'S HEADLINE SHAPE — the 200-iteration loop with the `?.`
// spelling inside an interpolation hole. The loop makes the leak decisive
// under LSan (200 x 32 B of header). The printed TOTAL is the observed value:
// each iteration renders the 36-character canonical Guid form, so the total is
// 200 x 36 = 7200. A program that leaked nothing but rendered the wrong
// string would print a different total.
const char* kLoopInterpolation =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var total := 0L; "
    "    var i := 0; "
    "    while i < 200 do "
    "      var g: Guid? := Guid.Empty; "
    "      total := total + long($\"{g?.ToString()}\".Length); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (2) THE LOCAL-BINDING SHAPE — `var s: string? := g?.ToString();`. The exit
// code is the LENGTH of the rendered canonical form (36), so the row asserts
// the VALUE as well as the cleanliness.
const char* kGuidLocalBinding =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var g: Guid? := Guid.Empty; "
    "    var s: string? := g?.ToString(); "
    "    Console.WriteLine($\"{s}\"); "
    "    return s?.Length ?? 0; "
    "  end Main; "
    "end C;";

// (3) THE `?.` FORMAT SPELLING — `g?.ToString("N")` (stdlib §2.18's "N"
// rendering: the canonical form with no hyphens). 32 characters.
const char* kGuidFormatLocalBinding =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var g: Guid? := Guid.Empty; "
    "    var s: string? := g?.ToString(\"N\"); "
    "    Console.WriteLine($\"{s}\"); "
    "    return s?.Length ?? 0; "
    "  end Main; "
    "end C;";

// (4) THE DateTime? SHAPE — the ISO-8601 round-trippable rendering
// (stdlib §2.5: "yyyy-MM-ddTHH:mm:ss"). Ticks 0 is 0001-01-01T00:00:00.
// The INTERPOLATION HOLE reads the `?.` producer a second time (and drops
// the fresh string in the hole), so this row exercises BOTH consumers of a
// `?.` producer: the named local and the hole.
const char* kDateTimeLocalBinding =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var d: DateTime? := DateTime.FromTicks(0L); "
    "    var s: string? := d?.ToString(); "
    "    Console.WriteLine($\"{s}\"); "
    "    Console.WriteLine($\"{d?.ToString()}\".Length); "
    "    return s?.Length ?? 0; "
    "  end Main; "
    "end C;";

// (5) THE REST OF THE §2.5 FAMILY — TimeSpan (the constant "c" form),
// DateOnly ("yyyy-MM-dd") and TimeOnly ("HH:mm:ss"). The exit code is the
// concatenation's length, so the row asserts the rendered TEXT as well.
const char* kFamilyLocalBinding =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var t: TimeSpan? := TimeSpan.FromTicks(5L); "
    "    var a: string? := t?.ToString(); "
    "    var d: DateOnly? := new DateOnly(2024, 3, 15); "
    "    var b: string? := d?.ToString(); "
    "    var u: TimeOnly? := new TimeOnly(1, 2, 3, 4); "
    "    var c: string? := u?.ToString(); "
    "    Console.WriteLine($\"{a}\"); "
    "    Console.WriteLine($\"{b}\"); "
    "    Console.WriteLine($\"{c}\"); "
    "    return (a?.Length ?? 0) + (b?.Length ?? 0) + (c?.Length ?? 0); "
    "  end Main; "
    "end C;";

// (6) THE CONTROL — the `.` spelling of the same read. It was CLEAN on the
// parent of this fix and MUST stay clean: the fix must not change the `.`
// spelling's ownership (a second release there would be a double free, which
// ASan reports as a heap-use-after-free / double free, not as a leak).
const char* kDotSpellingControl =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var g := Guid.Empty; "
    "    var s: string? := g.ToString(); "
    "    Console.WriteLine($\"{s}\"); "
    "    return s?.Length ?? 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) THE ISSUE'S HEADLINE — the 200-iteration `?.` interpolation loop is
// LSan-clean and prints the exact total (200 x 36 = 7200).
// stdlib §2.6 `Guid` ("`ToString()`: string" — canonical form) / §7.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           nullcond_guid_tostring_interpolation_loop_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kLoopInterpolation);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LeakSanitizer: 6400 B in 200 objects
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7200\n");
}

// =====================================================================
// (B) THE LOCAL-BINDING SHAPE — `var s: string? := g?.ToString();` is
// LSan-clean and the rendered value is the canonical 36-character form.
// stdlib §2.6 `Guid` / §7.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           nullcond_guid_tostring_local_binding_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kGuidLocalBinding);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LeakSanitizer: 32 B in 1 object
    BEZEL_EXPECT_EQ(r.exitCode, 36);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00000000-0000-0000-0000-000000000000\n");
}

// =====================================================================
// (C) THE `?.` FORMAT SPELLING — `g?.ToString("N")` renders the "N" form
// (no hyphens) and does not leak.
// stdlib §2.6 `Guid` / §2.18 (the "N" format code) / §7.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           nullcond_guid_tostring_format_lsan_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kGuidFormatLocalBinding);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LeakSanitizer: 32 B in 1 object
    BEZEL_EXPECT_EQ(r.exitCode, 32);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00000000000000000000000000000000\n");
}

// =====================================================================
// (D) THE DateTime? SHAPE — the ISO-8601 round-trippable rendering, and no
// leak. stdlib §2.5 `DateTime` / §7.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           nullcond_datetime_tostring_local_binding_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(kDateTimeLocalBinding);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LeakSanitizer: 32 B in 1 object
    BEZEL_EXPECT_EQ(r.exitCode, 19);
    BEZEL_EXPECT_EQ(r.stdoutText, "0001-01-01T00:00:00\n19\n");
}

// =====================================================================
// (E) THE REST OF THE §2.5 FAMILY — TimeSpan / DateOnly / TimeOnly, each
// read through `?.`, each rendering its §2.5 form, none leaking.
// stdlib §2.5 `TimeSpan`, `DateOnly`, `TimeOnly` / §7.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           nullcond_timespan_dateonly_timeonly_lsan_clean_stdlib_2_5) {
    JitResult r = runAotChecked(kFamilyLocalBinding);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: LeakSanitizer: 3 x 32 B
    // "00:00:00.0000005" (16) + "2024-03-15" (10) + "01:02:03" (8)
    BEZEL_EXPECT_EQ(r.exitCode, 34);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00:00:00.0000005\n2024-03-15\n01:02:03\n");
}

// =====================================================================
// (F) THE CONTROL — the `.` spelling stays clean AND keeps its value: the
// fix must not release the `.` spelling's producer reference a second time
// (a double free, which ASan reports as a use-after-free, not as a leak).
// stdlib §2.6 `Guid` / §28.9.
// =====================================================================
BEZEL_TEST(codegen_nullcond_value_builtin_tostring_leak_7953,
           dot_spelling_control_stays_clean_stdlib_2_6) {
    JitResult r = runAotChecked(kDotSpellingControl);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 36);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00000000-0000-0000-0000-000000000000\n");
}
