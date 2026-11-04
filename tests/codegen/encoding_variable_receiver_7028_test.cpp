// fix #7028 -- codegen: Encoding.GetBytes/GetString/GetByteCount through an
// `Encoding`-typed VALUE (stdlib §3.2, v0.355).
//
// stdlib §3.2 (v0.340) declares `Encoding` as an ABSTRACT class whose static
// properties UTF8/UTF16/UTF32/ASCII "return singleton instances of
// implementation-provided concrete subclasses ... callers access them through
// the `Encoding` base type". So binding one to a local, a parameter or a
// field and calling through it is the DOCUMENTED way to reach the operation —
// and it had NO lowering arm at all ("`GetBytes` on built-in type `Encoding`
// produced no value, and no dispatch arm emitted any code for this call",
// #2457's backstop).
//
// Since fix #7001 an `Encoding` value is the i32 KIND TAG a builtin
// class-typed slot lowers to (0 UTF8, 1 UTF16, 2 UTF32, 3 ASCII), so the
// variable receiver dispatches on that tag at RUN TIME to the same
// per-encoding lowering the literal `Encoding.UTF8.…` shape uses.
//
// Rows: the four singletons (byte counts + a GetString round trip), an
// encoding chosen at RUN TIME, an encoding passed as a PARAMETER, the
// two-argument `GetBytes(s, destination)` form, the ASCII §3.2 fault, a 100x
// allocating loop (so the ASan/LSan leg sees every allocation), and a control
// row pinning the literal shape.
//
// ⭐ THE TWO ORACLES. The rows that call `GetString` use `runAot` (ASan, but
// NOT the leak detector); every other row uses `runAotChecked` (ASan +
// LeakSanitizer). The reason is the compiler's DOCUMENTED string lifetime
// model: "a string produced by a call is conservatively left leaked by the
// codegen (the documented string lifetime model — see string_refcount_test.cpp)"
// — the same reason tests/codegen/encoding_test.cpp asserts no UAF /
// double-free rather than leak-freedom for the GetString path. That leak is
// PRE-EXISTING (it is the LITERAL shape's own behaviour, unchanged by this
// fix), so the rows that exercise it must not run the leak detector; every
// other row — the ones that allocate an owned `Array<byte>` — runs under it.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// The four singletons as stdlib §3.2 declares them, bound to `Encoding`
// locals — the shape this issue is about.
const char* kFour =
    "var a: Encoding := Encoding.UTF8; "
    "var b16: Encoding := Encoding.UTF16; "
    "var b32: Encoding := Encoding.UTF32; "
    "var asc: Encoding := Encoding.ASCII; ";
}  // namespace

// The issue's own repro: `var e: Encoding := Encoding.UTF8; var b :=
// e.GetBytes("hé");` failed to compile at all.
BEZEL_TEST(encoding_variable_receiver_7028, local_utf8_getbytes_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var e: Encoding := Encoding.UTF8; "
        "  var s := \"h\\u00E9\"; "
        "  using var b := e.GetBytes(s); "
        "  Console.WriteLine(b.Count); "
        "  Console.WriteLine(e.GetByteCount(s)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "hé" is 2 code points: 3 bytes in UTF-8.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n3\n"));
}

// The GetString round trip of the same shape (stdlib §3.2: `GetBytes` then
// `GetString` returns the original string).
BEZEL_TEST(encoding_variable_receiver_7028, local_utf8_getstring_roundtrip_3_2) {
    auto r = runAot(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var e: Encoding := Encoding.UTF8; "
        "  var s := \"h\\u00E9\"; "
        "  using var b := e.GetBytes(s); "
        "  Console.WriteLine(e.GetString(b).Length); "
        "  Console.WriteLine(e.GetString(b).Equals(s)); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\nTrue\n"));
}

// Every encoding stdlib §3.2 lists, with its byte count.
BEZEL_TEST(encoding_variable_receiver_7028, local_all_four_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        + std::string(kFour) +
        "  using var ab := a.GetBytes(\"h\\u00E9\"); "
        "  using var a16 := b16.GetBytes(\"h\\u00E9\"); "
        "  using var a32 := b32.GetBytes(\"h\\u00E9\"); "
        "  using var ascb := asc.GetBytes(\"abc\"); "
        "  Console.WriteLine(ab.Count); "
        "  Console.WriteLine(a16.Count); "
        "  Console.WriteLine(a32.Count); "
        "  Console.WriteLine(ascb.Count); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // UTF-8 3 bytes, UTF-16LE 4, UTF-32LE 8, US-ASCII 3.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n4\n8\n3\n"));
}

// The round trip of every encoding (stdlib §3.2: `GetString` decodes what
// `GetBytes` encoded).
BEZEL_TEST(encoding_variable_receiver_7028, local_all_four_roundtrip_3_2) {
    auto r = runAot(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        + std::string(kFour) +
        "  using var ab := a.GetBytes(\"h\\u00E9\"); "
        "  using var a16 := b16.GetBytes(\"h\\u00E9\"); "
        "  using var a32 := b32.GetBytes(\"h\\u00E9\"); "
        "  using var ascb := asc.GetBytes(\"abc\"); "
        "  Console.WriteLine(a.GetString(ab).Equals(\"h\\u00E9\")); "
        "  Console.WriteLine(b16.GetString(a16).Equals(\"h\\u00E9\")); "
        "  Console.WriteLine(b32.GetString(a32).Equals(\"h\\u00E9\")); "
        "  Console.WriteLine(asc.GetString(ascb).Equals(\"abc\")); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nTrue\nTrue\n"));
}

// stdlib §3.2's fault model through the variable shape: ASCII is strict
// seven-bit, so `GetBytes` raises FormatException on a code point above
// U+007F.
BEZEL_TEST(encoding_variable_receiver_7028, ascii_fault_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var asc: Encoding := Encoding.ASCII; "
        "  var caught := 0; "
        "  try "
        "    using var bad := asc.GetBytes(\"h\\u00E9\"); "
        "    Console.WriteLine(\"no fault\"); "
        "  catch FormatException => caught := caught + 1; "
        "  end; "
        "  Console.WriteLine(caught); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The encoding is chosen at RUN TIME: the branch reads a byte count out of a
// runtime-produced array, so the compiler cannot fold it.
BEZEL_TEST(encoding_variable_receiver_7028, runtime_chosen_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var e: Encoding := Encoding.UTF8; "
        "  using var probe := e.GetBytes(\"x\"); "
        "  if probe.Count > 0 then e := Encoding.UTF32; end; "
        "  Console.WriteLine(e.GetByteCount(\"h\\u00E9\")); "
        "  using var b := e.GetBytes(\"h\\u00E9\"); "
        "  Console.WriteLine(b.Count); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // UTF-32LE: 2 code points x 4 bytes.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n8\n"));
}

// The encoding arrives as a PARAMETER.
BEZEL_TEST(encoding_variable_receiver_7028, parameter_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C "
        "  static method Count(e: Encoding, s: string): int "
        "    return e.GetByteCount(s); "
        "  end Count; "
        "  public static method Main(): int "
        + std::string(kFour) +
        "  Console.WriteLine(Count(a, \"h\\u00E9\")); "
        "  Console.WriteLine(Count(b16, \"h\\u00E9\")); "
        "  Console.WriteLine(Count(b32, \"h\\u00E9\")); "
        "  Console.WriteLine(Count(asc, \"abc\")); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n4\n8\n3\n"));
}

// The two-argument `GetBytes(s, destination: Span<byte>)` form (stdlib §3.2 / §4.14),
// through the variable shape.
BEZEL_TEST(encoding_variable_receiver_7028, destination_span_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var e: Encoding := Encoding.UTF8; "
        "  using var dst := new Array<byte>(8, 0); "
        "  Console.WriteLine(e.GetBytes(\"h\\u00E9\", dst.AsSpan())); "
        "  Console.WriteLine(dst[0]); "
        "  Console.WriteLine(dst[1]); "
        "  Console.WriteLine(dst[2]); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // U+00E9 is C3 A9 in UTF-8; the first byte is 'h' = 104.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n104\n195\n169\n"));
}

// 100 iterations, each allocating an owned `Array<byte>` — the leak-checking
// oracle sees every allocation (a leaked one shows up here).
BEZEL_TEST(encoding_variable_receiver_7028, loop_100_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  var e: Encoding := Encoding.UTF8; "
        "  var total := 0; "
        "  for var i in 0..99 do "
        "    using var b := e.GetBytes(\"h\\u00E9\"); "
        "    total := total + b.Count; "
        "  end; "
        "  Console.WriteLine(total); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

// Control: the literal shape keeps working, unchanged.
BEZEL_TEST(encoding_variable_receiver_7028, literal_shape_unchanged_3_2) {
    auto r = runAotChecked(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  Console.WriteLine(Encoding.UTF8.GetByteCount(\"h\\u00E9\")); "
        "  using var b := Encoding.UTF8.GetBytes(\"h\\u00E9\"); "
        "  Console.WriteLine(b.Count); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n3\n"));
}

// Control: the literal shape's GetString round trip, unchanged.
BEZEL_TEST(encoding_variable_receiver_7028, literal_shape_roundtrip_unchanged_3_2) {
    auto r = runAot(
        "module M; import System; import System.Text; "
        "class C public static method Main(): int "
        "  using var b := Encoding.UTF8.GetBytes(\"h\\u00E9\"); "
        "  Console.WriteLine(Encoding.UTF8.GetString(b).Equals(\"h\\u00E9\")); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}
