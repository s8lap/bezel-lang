// fix #6957 / #6920 leftover -- sema: builtin byte-buffer and Encoding source
// arguments (stdlib §10.3 RawBytes.From declares only Array<byte>; §10.1
// CString.FromBytes only Array<byte>; §3.2 GetBytes/GetByteCount take a
// string or ReadOnlyStringSpan; v0.340). A span argument to RawBytes.From
// passed sema and failed the IR verifier; Encoding.UTF8.GetBytes(5) likewise.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool rejectedWith(const bezel::test::JitResult& r, const std::string& needle) {
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
}  // namespace

BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, rawbytes_from_readonlyspan_10_3) {
    auto r = runAot(prog(
        "var a: Array<byte> := [1,2]; var sp: ReadOnlySpan<byte> := "
        "a.AsReadOnlySpan(); using var r := RawBytes.From(sp);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`RawBytes.From` expects `Array<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, cstring_frombytes_readonlyspan_10_1) {
    auto r = runAot(prog(
        "var a: Array<byte> := [1,2]; var sp: ReadOnlySpan<byte> := "
        "a.AsReadOnlySpan(); using var c := CString.FromBytes(sp);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`CString.FromBytes` expects `Array<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, encoding_getbytes_int_3_2) {
    auto r = runAot(prog("var b := Encoding.UTF8.GetBytes(5);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`Encoding.GetBytes` expects a `string`"));
}
BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, encoding_getbytecount_int_3_2) {
    auto r = runAot(prog("var n := Encoding.UTF8.GetByteCount(5);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`Encoding.GetByteCount` expects a `string`"));
}
BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, encoding_getstring_array_int_3_2) {
    auto r = runAot(prog(
        "var a: Array<int> := [104, 105]; "
        "Console.WriteLine(Encoding.UTF8.GetString(a));"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "got `Array<int>`"));
}
// Controls: every declared overload still compiles and prints exact output.
BEZEL_TEST(sema_builtin_byte_span_encode_arg_6957, legal_overloads_still_accepted) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := [104, 105]; "
        "  var sp: ReadOnlySpan<byte> := b.AsReadOnlySpan(); "
        "  var asp: ReadOnlyArraySpan<byte> := b.AsReadOnlySpan(); "
        "  Console.WriteLine(Encoding.UTF8.GetString(b)); "
        "  Console.WriteLine(Encoding.UTF8.GetString(sp)); "
        "  Console.WriteLine(Encoding.UTF8.GetString(asp)); "
        "  var e := Encoding.UTF8.GetBytes(\"hey\"); "
        "  Console.WriteLine(e.Count); "
        "  Console.WriteLine(Encoding.UTF8.GetByteCount(\"hey\")); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("hi\nhi\nhi\n3\n3\n"));
}
