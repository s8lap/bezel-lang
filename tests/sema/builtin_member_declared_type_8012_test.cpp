// fix #8012 (stdlib §2.5, §5.1, v0.350) — the §5.1 ADMISSION at a built-in
// MEMBER position whose DECLARED type is a RECORD.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §2.5 "DateTime, TimeSpan, DateOnly, TimeOnly" declares, verbatim:
//
//   public method CompareTo(other: DateTime): int;
//   public method Add(span: TimeSpan): DateTime;
//   public method Subtract(other: DateTime): TimeSpan;
//   public method Subtract(span: TimeSpan): DateTime;
//   public method CompareTo(other: TimeSpan): int;
//   public method CompareTo(other: DateOnly): int;
//   public method ToDateTime(time: TimeOnly): DateTime;
//   public method CompareTo(other: TimeOnly): int;
//
// spec §5.1 "Implicit conversions": the implicit conversions are the
// identity, the widening table, and the CONSTANT narrowing — "An expression
// of integer type that is a compile-time constant implicitly converts to any
// integer type whose range contains its value".  Nothing else converts
// implicitly, so an `int` reaches a declared `TimeSpan` parameter by NO
// conversion at all.
//
// spec §22.6 "Overload resolution": a call that matches no declared overload
// is a compile error and the diagnostic cites the candidates it examined.
//
// --- THE DEFECT (MEASURED on this lane's base) ------------------------------
//
// The ONE built-in parameter table (`src/common/BuiltinRegistry.cpp`'s
// `kBuiltinParams`) carries, per DECLARED FORM, the parameter NAME (§22.5),
// the parameter TYPE (§5.1) and the parameter NULLABILITY (§7.3).  Its
// `kinds` column is a BIT SET over a fixed vocabulary — the scalar kinds and
// a few named ones — and that vocabulary names NO bit for a record.  So the
// EIGHT §2.5 member positions above carried NO type opinion at all
// (`kBPTAny`) and the §5.1 gate SKIPPED them: `dt.Add(5)` — an `int` at a
// declared `TimeSpan` parameter — COMPILED, and so did `dt.Subtract(5)`,
// `dt.CompareTo(5)`, `ts.CompareTo(5)`, `d.CompareTo(5)`, `d.ToDateTime(5)`
// and `t.CompareTo(5)`.
//
// --- THE FIX ----------------------------------------------------------------
//
// The declared type is NAMED, not bit-coded: the gate carries "one declared
// type per position" (`kBuiltinMemberDeclaredTypes` in
// `src/sema/ExprTypecheck_Calls.cpp`), and a position's declared OVERLOAD
// SET (stdlib §2.5 declares `Subtract(other: DateTime)` BESIDE
// `Subtract(span: TimeSpan)`) is a SET of those names.  The table's `kinds`
// column is not edited — its vocabulary cannot spell a record.
//
// The rows below pin, each with the EXACT diagnostic:
//   * the seven §2.5 positions, refused with the §5.1 reason;
//   * the LEGAL spellings — every §2.5 declared overload — still admitted.
//
// Bezel syntax notes: `Console.WriteLine` prints an `int` directly.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A program over the §2.5 temporal records.
std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const char* needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, needle);
}

// The §22.6 refusal the §5.1 gate produces: "no matching overload of `X` —
// stdlib §2.5 declares `...`, so the argument at position 0 must be a `T` —
// a `int` is never implicitly convertible to a `T` (§5.1)".
std::string overloadRefusal(const char* member, const char* declares,
                            const char* must) {
    return std::string("no matching overload of `") + member +
           "` — stdlib " "\xC2\xA7" "2.5 declares `" + declares +
           "`, so the argument at position 0 must be " + must +
           " — a `int` is never implicitly convertible to " + must + " (" "\xC2\xA7" "5.1)";
}

}  // namespace

// --- THE SEVEN §2.5 POSITIONS: a wrong type is REFUSED ----------------------

// stdlib §2.5 declares `Add(span: TimeSpan): DateTime` — an `int` is not a
// `TimeSpan` and §5.1 makes no conversion of one into the other.
BEZEL_TEST(builtin_member_declared_type_8012, datetime_add_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var dt := new DateTime(2024, 1, 2); "
             "    const x := dt.Add(5);"),
        overloadRefusal("DateTime.Add", "Add(span: TimeSpan): DateTime",
                        "a `TimeSpan`").c_str()));
}

// stdlib §2.5 declares BOTH `Subtract(other: DateTime): TimeSpan` and
// `Subtract(span: TimeSpan): DateTime`, so the position admits a `DateTime`
// OR a `TimeSpan` — and nothing else.
BEZEL_TEST(builtin_member_declared_type_8012,
           datetime_subtract_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var dt := new DateTime(2024, 1, 2); "
             "    const x := dt.Subtract(5);"),
        overloadRefusal("DateTime.Subtract",
                        "Subtract(other: DateTime): TimeSpan` / "
                        "`Subtract(span: TimeSpan): DateTime",
                        "a `DateTime` or a `TimeSpan`").c_str()));
}

// stdlib §2.5 declares `CompareTo(other: DateTime): int`.
BEZEL_TEST(builtin_member_declared_type_8012,
           datetime_compareto_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var dt := new DateTime(2024, 1, 2); "
             "    const x := dt.CompareTo(5);"),
        overloadRefusal("DateTime.CompareTo", "CompareTo(other: DateTime): int",
                        "a `DateTime`").c_str()));
}

// stdlib §2.5 declares `CompareTo(other: TimeSpan): int`.
BEZEL_TEST(builtin_member_declared_type_8012,
           timespan_compareto_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var ts := TimeSpan.FromSeconds(5); "
             "    const x := ts.CompareTo(5);"),
        overloadRefusal("TimeSpan.CompareTo", "CompareTo(other: TimeSpan): int",
                        "a `TimeSpan`").c_str()));
}

// stdlib §2.5 declares `CompareTo(other: DateOnly): int`.
BEZEL_TEST(builtin_member_declared_type_8012,
           dateonly_compareto_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var d := new DateOnly(2024, 1, 2); "
             "    const x := d.CompareTo(5);"),
        overloadRefusal("DateOnly.CompareTo", "CompareTo(other: DateOnly): int",
                        "a `DateOnly`").c_str()));
}

// stdlib §2.5 declares `ToDateTime(time: TimeOnly): DateTime`.
BEZEL_TEST(builtin_member_declared_type_8012,
           dateonly_todatetime_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var d := new DateOnly(2024, 1, 2); "
             "    const x := d.ToDateTime(5);"),
        overloadRefusal("DateOnly.ToDateTime", "ToDateTime(time: TimeOnly): DateTime",
                        "a `TimeOnly`").c_str()));
}

// stdlib §2.5 declares `CompareTo(other: TimeOnly): int`.
BEZEL_TEST(builtin_member_declared_type_8012,
           timeonly_compareto_int_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    var t := new TimeOnly(1, 2, 3); "
             "    const x := t.CompareTo(5);"),
        overloadRefusal("TimeOnly.CompareTo", "CompareTo(other: TimeOnly): int",
                        "a `TimeOnly`").c_str()));
}

// --- THE CONTROLS: every declared §2.5 overload still works -----------------

// Every §2.5 member position, called the way stdlib §2.5 declares it, still
// compiles and runs.  These rows are GREEN before the fix and GREEN after it:
// they are the admitting twins of the refusals above, so they show the
// refusals fire on the TYPE and not on the shape of the call.
BEZEL_TEST(builtin_member_declared_type_8012, declared_overloads_are_admitted) {
    // `Add(span: TimeSpan)` and BOTH `Subtract` overloads.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var dt := new DateTime(2024, 1, 2); "
        "    var ts := TimeSpan.FromSeconds(3600); "
        "    Console.WriteLine(dt.Add(ts).Hour); "
        "    Console.WriteLine(dt.Subtract(ts).Hour); "
        "    Console.WriteLine(dt.Subtract(dt).Ticks); ")),
        "1\n23\n0");
    // The four `CompareTo` overloads and `ToDateTime(time: TimeOnly)`.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var dt := new DateTime(2024, 1, 2); "
        "    var dt2 := new DateTime(2024, 3, 4); "
        "    var ts := TimeSpan.FromSeconds(3600); "
        "    var ts2 := TimeSpan.FromSeconds(7); "
        "    var d := new DateOnly(2024, 1, 2); "
        "    var d2 := new DateOnly(2024, 3, 4); "
        "    var t := new TimeOnly(1, 2, 3); "
        "    var t2 := new TimeOnly(4, 5, 6); "
        "    Console.WriteLine((if dt.CompareTo(dt2) < 0 then 1 else 0)); "
        "    Console.WriteLine((if ts.CompareTo(ts2) > 0 then 1 else 0)); "
        "    Console.WriteLine((if d.CompareTo(d2) < 0 then 1 else 0)); "
        "    Console.WriteLine((if t.CompareTo(t2) < 0 then 1 else 0)); "
        "    Console.WriteLine(d.ToDateTime(t).Hour); ")),
        "1\n1\n1\n1\n1");
}
