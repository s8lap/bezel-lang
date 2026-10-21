// fix #6920 -- sema: builtin byte-buffer parameters were not checked against their
// declared parameter type (stdlib §10.3 RawBytes.From, §10.1
// CString.FromBytes, §2.6 Guid.FromByteArray, §2.7 Random.NextBytes, §7.3
// File.WriteAllBytes, §3.2 Encoding.GetString; v0.340).
// Common cause: the typeCall arms type these members by NAME only and never
// compared the argument with the declared parameter, so a `string`/`int`/`Set`
// reached a runtime entry expecting a bz_array_t* (silent accept / IR failure).

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

BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, rawbytes_from_string_10_3) {
    auto r = runAot(prog("using var r := RawBytes.From(\"hello\");"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`RawBytes.From` expects `Array<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, rawbytes_from_int_10_3) {
    auto r = runAot(prog("using var r := RawBytes.From(42);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`RawBytes.From` expects `Array<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, rawbytes_from_set_10_3) {
    auto r = runAot(prog(
        "var s := new Set<byte>(); using var r := RawBytes.From(s);"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "got `Set<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, cstring_frombytes_string_10_1) {
    auto r = runAot(prog("using var c := CString.FromBytes(\"hello\");"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`CString.FromBytes` expects `Array<byte>`"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, encoding_getstring_string_3_2) {
    auto r = runAot(prog("Console.WriteLine(Encoding.UTF8.GetString(\"hello\"));"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`Encoding.GetString` expects"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, guid_frombytearray_string_2_6) {
    auto r = runAot(prog("var g := Guid.FromByteArray(\"0123456789abcdef\");"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`Guid.FromByteArray` expects"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, random_nextbytes_string_2_7) {
    auto r = runAot(prog("var r := new Random(1); r.NextBytes(\"hello\");"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`Random.NextBytes` expects"));
}
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, file_writeallbytes_string_7_3) {
    auto r = runAot(prog("File.WriteAllBytes(\"/var/tmp/x.bin\", \"hello\");"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "`File.WriteAllBytes` expects"));
}
// Control: the legal forms still compile and run.
BEZEL_TEST(sema_builtin_byte_buffer_arg_typed_6920, legal_byte_array_args_still_accepted) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := [104, 105]; "
        "  using var raw := RawBytes.From(b); "
        "  using var cs := CString.FromBytes(b); "
        "  if Encoding.UTF8.GetString(b) <> \"hi\" then return 1; end; "
        "  return raw.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}
