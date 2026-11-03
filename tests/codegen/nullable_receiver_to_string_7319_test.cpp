// Spec-derived regression test for issue #7319 -- `x.ToString(...)` on a
// NULLABLE receiver read the CARRIER instead of the PAYLOAD, and on a
// `double?` receiver the compiler emitted MALFORMED IR and aborted.
//
// THE DEFECT.
//
// A nullable receiver carries a TAGGED value: the payload plus a
// null/present flag. The `ToString` arms read the receiver's bytes as if the
// carrier were the payload, so the program printed the carrier's bytes:
//
//   var x: int? := 5;
//   Console.WriteLine($"[{x.ToString("F4")}]");
//       ->  [1901450753]        (the CARRIER's bytes, not "5.0000")
//
// and on a `double?` receiver the compiler ABORTED inside the LLVM verifier
// with malformed IR:
//
//   Invalid operand types for FCmp instruction
//     %ts.isnan = fcmp uno { i8, double } %11, %11
//   Both operands to FCmp instruction are not of the same type!
//     %ts.isposinf = fcmp oeq { i8, double } %11, double 0x7FF0000000000000
//
// THE DOCUMENTS.
//
// spec §17.4 (v0.350) and §7 (v0.350): a nullable carrier's read yields the
// PAYLOAD when the carrier is present -- the carrier is the language's
// representation of `T?`, and `T?`'s value IS the `T` it carries, not the
// carrier's bytes. spec §18.5 (v0.350) and stdlib §2.18 (v0.350) give the
// `ToString(format)` text: the "F4" format renders four fractional digits.
//
// WHAT FIXED IT.
//
// The defect was fixed by commit eacd9e03 (the #7319 lane's commit, "the
// one-argument ToString arm renders a nullable receiver's payload") and by
// commit 1d53a378 (the #7319 lane's later commit, "the DECLARATION-narrowed
// nullable receiver renders its PAYLOAD, not the carrier"). Their subjects'
// issue prefixes are left out of this message so that this message closes
// #7319 and nothing else, and neither of them closed the issue.
//
// WHAT THIS FILE ADDS. The two fixing commits' own test files --
// tests/codegen/nullable_receiver_tostring_format_7319_test.cpp and
// tests/codegen/nullable_receiver_tostring_payload_7319_test.cpp -- pin the
// receiver read UNDER AN EXPLICIT NULL TEST (`if x <> null then ...`). This
// file pins the DECLARATION-NARROWED local read with NO narrowing test at the
// call site, in both the formatted and the one-argument arm -- the shape the
// issue's own measurement table records ("And it is NOT ABOUT NARROWING --
// the unnarrowed form is equally wrong"). A parameter receiver is a
// different case (spec §7.4: a parameter is narrowed only by the forms §7.4
// lists), and the payload file's fourth row pins that refusal.
//
// THE ORACLE.
//
// Every row prints the rendered text and asserts the exact string. The
// receiver is a `T?` local that HOLDS a value, so the payload is present and
// the rendered text is the payload's -- a carrier read would print digits
// that no formatting of 5 can produce. The rows assert the CORRECT
// observable; there is no negative assertion on a diagnostic anywhere in
// this file.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class Program "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ===========================================================================
// THE PIN ROWS -- THE FORMATTED ARM, ON A `double?` RECEIVER.
//
// This is the shape the compiler ABORTED on (malformed IR), so on the
// pre-fix tree the program produces no value at all.
// ===========================================================================
BEZEL_TEST(nullable_receiver_to_string_7319,
           narrowed_double_nullable_receiver_renders_its_payload) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var y: double? := 5.0; "
            "    if y <> null then "
            "      var s: string := y.ToString(\"F4\"); "
            "      Console.WriteLine($\"[{s}]\"); "
            "    end if; ")),
        std::string("[5.0000]"));
}

// ===========================================================================
// THE PIN ROW -- THE FORMATTED ARM, ON AN `int?` RECEIVER, NARROWED.
//
// This is the shape that printed the CARRIER's bytes ("[1901450753]").
// ===========================================================================
BEZEL_TEST(nullable_receiver_to_string_7319,
           narrowed_int_nullable_receiver_renders_its_payload) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var x: int? := 5; "
            "    if x <> null then "
            "      var s: string := x.ToString(\"F4\"); "
            "      Console.WriteLine($\"[{s}]\"); "
            "    end if; ")),
        std::string("[5.0000]"));
}

// ===========================================================================
// THE PIN ROW -- THE FORMATTED ARM, UNNARROWED.
//
// The issue records that the unnarrowed form was equally wrong; the
// carrier's bytes are read the same way, so the row is not about the
// narrowing.
// ===========================================================================
BEZEL_TEST(nullable_receiver_to_string_7319,
           unnarrowed_int_nullable_receiver_renders_its_payload) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var x: int? := 5; "
            "    Console.WriteLine($\"[{x.ToString(\"F4\")}]\"); ")),
        std::string("[5.0000]"));
}

// ===========================================================================
// THE PIN ROW -- THE ONE-ARGUMENT ARM.
//
// `ToString()` with no format is the other arm of the family, and it is the
// one the issue's own table records as CORRECT only for the narrowed
// `int?` case. It must render the payload too.
// ===========================================================================
BEZEL_TEST(nullable_receiver_to_string_7319,
           unnarrowed_int_nullable_receiver_renders_its_payload_unformatted) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var x: int? := 5; "
            "    Console.WriteLine($\"[{x.ToString()}]\"); ")),
        std::string("[5]"));
}

// ===========================================================================
// THE CONTROL -- A NON-NULLABLE RECEIVER.
//
// The `int` receiver's rendering is unchanged by any of this, so the row is
// green on both arms; it is here so that the rows above cannot be satisfied
// by "the format is different".
// ===========================================================================
BEZEL_TEST(nullable_receiver_to_string_7319,
           non_nullable_receiver_renders_the_same_text) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var i: int := 5; "
            "    Console.WriteLine($\"[{i.ToString(\"F4\")}]\"); "
            "    Console.WriteLine($\"[{i.ToString()}]\"); ")),
        std::string("[5.0000]\n[5]"));
}
