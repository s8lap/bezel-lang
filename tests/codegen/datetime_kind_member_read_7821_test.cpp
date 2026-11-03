// Spec-derived regression test for issue #7821 — `Console.WriteLine(x.Kind)`
// on a `DateTime`'s `Kind` member printed NOTHING (the member read did not
// lower to a value, so the whole statement emitted nothing).
//
// THE SPEC.
//
//   stdlib §2.5 "DateTime, TimeSpan, DateOnly, TimeOnly" (v0.350)
//   declares:
//
//     public enum DateTimeKind: byte Unspecified, Utc, Local;
//     public constructor(year: int, month: int, day: int, hour: int,
//                        minute: int, second: int,
//                        millisecond: int := 0,
//                        kind: DateTimeKind := DateTimeKind.Unspecified);
//     public readonly property Kind: DateTimeKind;
//
//   spec §8.5 `ToString` (v0.350): "For body records that do not satisfy
//   auto-equality conditions, and for enums: the type's qualified name (the
//   stdlib §2.13 `FullName` rendering, as for classes)."  `DateTimeKind`
//   lives in `System` (stdlib §2.5), so the qualified name is
//   "System.DateTimeKind".
//
//   spec §11.3 / §12.1 (v0.350): a member read on a declared member
//   produces that member's VALUE.  A read that lowers to nothing is not a
//   value, and the statement that carries it emits nothing at all.
//
// THE DEFECT.
//
// `DateTime`'s runtime value is a bare tick count (an i64) in every runtime
// helper, so `Kind` had nowhere to live: reading `x.Kind` failed to lower
// to a value, the compiler reported "internal: failed to lower", and the
// whole `Console.WriteLine` statement emitted NOTHING — the program ran,
// exited 0, and printed no line at all.
//
// THE ORACLE IS POSITIVE.
//
// Every row asserts the EXACT text the program prints.  A read that is
// silently dropped prints nothing, so the expectation
// "System.DateTimeKind" fails on that tree — and so does the
// "True" expectation of the comparison row, whose operand never lowered.
//
// The rows are the issue's own repro shapes: the `kind` carried by a local,
// the `kind` carried by a NULLABLE local (which reaches the constructor
// through the nullable carrier), and the `kind` compared with the enum
// member it was supplied as.

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

// ===========================================================================
// THE MEMBER READ LOWERS TO A VALUE.
//
// The `kind` is supplied through a local, so the constructor's `kind`
// parameter is bound by §22.5's named-argument rule and the value carries
// it; reading `Kind` back prints the enum's qualified name (spec §8.5).
// ===========================================================================
BEZEL_TEST(datetime_kind_member_read_7821,
           datetime_kind_member_read_prints_the_qualified_type_name) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var k := DateTimeKind.Utc; "
        "    var x := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "    Console.WriteLine(x.Kind);")),
        std::string("System.DateTimeKind"));
}

// ===========================================================================
// THE SAME, WITH A NULLABLE `kind`.
//
// stdlib §2.5 declares the parameter as `DateTimeKind` (non-nullable), so
// the nullable local is narrowed at the call (§7.4) and the value still
// carries the kind.
// ===========================================================================
BEZEL_TEST(datetime_kind_member_read_7821,
           datetime_kind_member_read_through_a_nullable_kind) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var k: DateTimeKind? := DateTimeKind.Utc; "
        "    if k <> null then "
        "      var x := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "      Console.WriteLine(x.Kind); "
        "    end if; ")),
        std::string("System.DateTimeKind"));
}

// ===========================================================================
// THE READ IS A VALUE, NOT A DROP — it can be COMPARED with the enum
// member it was supplied as (spec §8.4's enum equality).
// ===========================================================================
BEZEL_TEST(datetime_kind_member_read_7821,
           datetime_kind_member_read_is_comparable_with_the_enum) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var k := DateTimeKind.Utc; "
        "    var x := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "    Console.WriteLine(x.Kind = DateTimeKind.Utc);")),
        std::string("True"));
}

// ===========================================================================
// THE READ AND ITS NEIGHBOUR — the `Kind` read and the `Millisecond` read
// of a second construction that names the defaulted `millisecond`
// parameter (§22.5).  Both lines must be present: a dropped `Kind` read
// leaves only the second line, so the two-line expectation catches it.
// ===========================================================================
BEZEL_TEST(datetime_kind_member_read_7821,
           datetime_kind_member_read_keeps_its_neighbouring_read) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var k: DateTimeKind := DateTimeKind.Utc; "
        "    var x := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "    Console.WriteLine(x.Kind); "
        "    var y := new DateTime(2020, 1, 1, 1, 1, 1, millisecond := 5, "
        "kind := k); "
        "    Console.WriteLine(y.Millisecond);")),
        std::string("System.DateTimeKind\n5"));
}
