// Spec-derived regression test for issue #6957 -- a built-in member arm that
// did not check its ARGUMENT against the declared parameter type.
//
// THE DEFECT.
//
// stdlib §3.2 (v0.350) declares `Encoding.GetBytes(s: string): owned
// Array<byte>` and `Encoding.GetByteCount(s: string): int`, and stdlib
// §10.1 / §10.3 declare `CString.FromBytes(bytes: Array<byte>)` and
// `RawBytes.From(buffer: Array<byte>)`. None of them declares an
// `Array<int>`, an `Array<char>` or a byte-SPAN form, and spec §5.1 (v0.350)
// has no conversion from those to the declared parameter types. So such a
// call has NO applicable overload and is an error under spec §22.6 (v0.350)
// -- but the arms took whatever they were handed.
//
// THE DOCUMENTS.
//
// spec §5.1 (v0.350): a conversion is applied where the target type is
// known, and only over the table the section lists. spec §22.6 (v0.350): a
// call that matches no declared form is a compile error, reported at the
// call. stdlib §3.2 (v0.350) declares the `Encoding` byte-source forms;
// stdlib §10.1 (v0.350) and stdlib §10.3 (v0.350) declare the `CString`
// and `RawBytes` factories.
//
// WHAT FIXED IT.
//
// The `Encoding.GetBytes` / `Encoding.GetByteCount` arms were fixed by
// commit 654c49eb (the #6957 lane's commit, "reject span args to
// RawBytes.From/CString.FromBytes and non-string Encoding.GetBytes/
// GetByteCount sources"); its subject's issue prefix is left out of this
// message so that this message closes #6957 and nothing else. The
// `RawBytes.From` / `CString.FromBytes` span arms and the `Encoding.GetString`
// arm were fixed by the sibling #6920 lane's commit, which the issue's body
// worried "may not cover this one" -- it does. Neither commit closed the
// issue, so it stayed OPEN until this commit.
//
// THE ORACLE.
//
// The rejection rows assert the CORRECT observable: the program does NOT run,
// and the driver's diagnostic names the member and the expected parameter
// type and cites the section that declares it. The control row asserts that
// the CONFORMING spellings still work, so a fix that closed the whole
// surface would not pass. There is no negative assertion on a diagnostic
// anywhere in this file beyond the positive presence checks above.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The rejection: the program did not run, the driver rejected it at a REAL
// source location (never a crash / signal / timeout), and the diagnostic
// names the member and the expected parameter type.
void expectArgumentRefused(const JitResult& r, const char* member,
                            const char* expectedType, const char* section) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, member));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, expectedType));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, section));
}

}  // namespace

// ===========================================================================
// THE PIN ROW -- `Encoding.GetBytes` AT A SOURCE IT DOES NOT DECLARE.
//
// stdlib §3.2 declares `GetBytes(s: string)` and the span-source forms;
// an `Array<int>` is neither, and §5.1 has no conversion to one.
// ===========================================================================
BEZEL_TEST(encoding_source_arg_type_6957,
           encoding_get_bytes_at_an_array_int_source_is_refused_3_2) {
    auto r = runAot(prog(
        "    var a: Array<int> := [104, 105]; "
        "    Console.WriteLine(Encoding.UTF8.GetBytes(a).Count); "));
    expectArgumentRefused(r, "Encoding.GetBytes", "string", "§3.2");
}

// ===========================================================================
// THE PIN ROW -- `Encoding.GetByteCount` AT A SOURCE IT DOES NOT DECLARE.
//
// The sibling arm of the pair the issue names.
// ===========================================================================
BEZEL_TEST(encoding_source_arg_type_6957,
           encoding_get_byte_count_at_an_array_byte_source_is_refused_3_2) {
    auto r = runAot(prog(
        "    var a: Array<byte> := [104, 105]; "
        "    Console.WriteLine(Encoding.UTF8.GetByteCount(a)); "));
    expectArgumentRefused(r, "Encoding.GetByteCount", "string", "§3.2");
}

// ===========================================================================
// THE PIN ROW -- `RawBytes.From` AT A SPAN.
//
// stdlib §10.3 declares `RawBytes.From(buffer: Array<byte>): owned
// RawBytes`; there is no span form, and §5.1 has no conversion from a
// `ReadOnlySpan<byte>` to an `Array<byte>`.
// ===========================================================================
BEZEL_TEST(encoding_source_arg_type_6957,
           raw_bytes_from_at_a_span_source_is_refused_10_3) {
    auto r = runAot(prog(
        "    var b: Array<byte> := [104, 105]; "
        "    var s := new ReadOnlySpan<byte>(b); "
        "    Console.WriteLine(RawBytes.From(s).Length); "));
    expectArgumentRefused(r, "RawBytes.From", "Array<byte>", "§10.3");
}

// ===========================================================================
// THE PIN ROW -- `CString.FromBytes` AT A SPAN.
//
// stdlib §10.1 declares `CString.FromBytes(bytes: Array<byte>): owned
// CString`; the twin of the row above.
// ===========================================================================
BEZEL_TEST(encoding_source_arg_type_6957,
           c_string_from_bytes_at_a_span_source_is_refused_10_1) {
    auto r = runAot(prog(
        "    var b: Array<byte> := [104, 105]; "
        "    var s := new ReadOnlySpan<byte>(b); "
        "    Console.WriteLine(CString.FromBytes(s).Length); "));
    expectArgumentRefused(r, "CString.FromBytes", "Array<byte>", "§10.1");
}

// ===========================================================================
// THE CONTROL -- THE CONFORMING SPELLINGS.
//
// stdlib §3.2's `GetByteCount(s: string)` and `GetBytes(s: string)` must
// still work; the row is green on both arms, so the rejection rows above
// cannot be satisfied by "the arm was closed".
// ===========================================================================
BEZEL_TEST(encoding_source_arg_type_6957,
           conforming_string_sources_still_work_3_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(Encoding.UTF8.GetByteCount(\"hi\")); "
            "    Console.WriteLine(Encoding.UTF8.GetBytes(\"hi\").Count); ")),
        std::string("2\n2"));
}
