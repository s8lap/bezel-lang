// Spec-derived tests for fix #1355 — Encoding.GetByteCount and
// Encoding.GetBytes(s, destination: Span<byte>), the two new overloads added in
// stdlib §3.2 v0.165, plus the GetString(bytes, index, count) window contract.
//
// Authority: stdlib §3.2 v0.165:
//   public abstract method GetByteCount(s: string): int;
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//       // returns bytes written; IndexOutOfRangeException if destination too short
//   public abstract method GetString(bytes: Array<byte>, index: int, count: int): string;
//       // window over Count; IndexOutOfRangeException out of range
// Error model (v0.136, unchanged): malformed/unrepresentable input is a defined
// fault — ASCII.GetByteCount/GetBytes raise FormatException on any code point
// above U+007F.
//
// Bezel syntax: inequality `<>` (§17.3); byte value `byte(N)`; an uncaught
// runtime raise would exit-70 the whole binary, so raises are pinned in-source
// with `try ... catch <Type> => ... end`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── GetByteCount: one byte-count per encoding ─────────────────────────────────

BEZEL_TEST(encoding_getbytecount_span_1355, utf8_ascii_count) {
    // per stdlib §3.2 v0.165 (#1355): "hi" is 2 UTF-8 bytes.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  return Encoding.UTF8.GetByteCount(\"hi\"); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(encoding_getbytecount_span_1355, utf8_multibyte_count) {
    // per stdlib §3.2 v0.165 (#1355): U+00E9 encodes to 2 UTF-8 bytes.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  return Encoding.UTF8.GetByteCount(\"\\u00e9\"); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(encoding_getbytecount_span_1355, utf16_count) {
    // per stdlib §3.2 v0.165 (#1355): UTF-16LE is 2 bytes per BMP code point.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  return Encoding.UTF16.GetByteCount(\"hi\"); "
        "end Main; end C;"), 4);
}

BEZEL_TEST(encoding_getbytecount_span_1355, utf32_count) {
    // per stdlib §3.2 v0.165 (#1355): UTF-32LE is 4 bytes per code point.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  return Encoding.UTF32.GetByteCount(\"hi\"); "
        "end Main; end C;"), 8);
}

BEZEL_TEST(encoding_getbytecount_span_1355, ascii_count) {
    // per stdlib §3.2 v0.165 (#1355): ASCII is 1 byte per code point.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  return Encoding.ASCII.GetByteCount(\"hi\"); "
        "end Main; end C;"), 2);
}

// GetByteCount agrees with the length of the array GetBytes(s) produces.
BEZEL_TEST(encoding_getbytecount_span_1355, count_matches_getbytes_length) {
    // per stdlib §3.2 v0.165 (#1355)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var n := Encoding.UTF8.GetByteCount(\"hello\"); "
        "  using var b := Encoding.UTF8.GetBytes(\"hello\"); "
        "  if n <> b.Count then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── GetBytes(s, destination: Span<byte>) — writes into an existing span ───────

BEZEL_TEST(encoding_getbytecount_span_1355, getbytes_span_returns_count_and_writes) {
    // per stdlib §3.2 v0.165 (#1355): returns bytes written; "Hi" → 'H','i'.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var dst := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"Hi\", dst.AsSpan()); "
        "  if n <> 2 then return 1; end; "
        "  if int(dst[0]) <> 72 then return 2; end; "   // 'H'
        "  if int(dst[1]) <> 105 then return 3; end; "  // 'i'
        "  return 0; "
        "end Main; end C;"), 0);
}

// A larger-than-needed destination is fine; only `n` bytes are written.
BEZEL_TEST(encoding_getbytecount_span_1355, getbytes_span_larger_dest_ok) {
    // per stdlib §3.2 v0.165 (#1355)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var dst := new Array<byte>(64, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"abc\", dst.AsSpan()); "
        "  return n; "
        "end Main; end C;"), 3);
}

// A too-short destination raises IndexOutOfRangeException (§3.2 v0.165).
BEZEL_TEST(encoding_getbytecount_span_1355, getbytes_span_too_short_raises) {
    // per stdlib §3.2 v0.165 (#1355)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var dst := new Array<byte>(1, 0); "  // needs 2, has 1
        "  try "
        "    var n := Encoding.UTF8.GetBytes(\"Hi\", dst.AsSpan()); "
        "    return 0; "
        "  catch IndexOutOfRangeException => return 7; "
        "  end; "
        "end Main; end C;"), 7);
}

// ── ASCII strict error model (§3.2 v0.136): non-ASCII → FormatException ───────

BEZEL_TEST(encoding_getbytecount_span_1355, ascii_getbytecount_nonascii_raises_format) {
    // per stdlib §3.2 v0.136/v0.165 (#1355): U+00E9 > U+007F → FormatException.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; "
        "class C public static method Main(): int "
        "  try "
        "    var n := Encoding.ASCII.GetByteCount(\"\\u00e9\"); "
        "    return 0; "
        "  catch FormatException => return 5; "
        "  end; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(encoding_getbytecount_span_1355, ascii_getbytes_span_nonascii_raises_format) {
    // per stdlib §3.2 v0.136/v0.165 (#1355)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var dst := new Array<byte>(8, 0); "
        "  try "
        "    var n := Encoding.ASCII.GetBytes(\"\\u00e9\", dst.AsSpan()); "
        "    return 0; "
        "  catch FormatException => return 5; "
        "  end; "
        "end Main; end C;"), 5);
}

// Pure-ASCII GetBytes into a span succeeds (the fault only fires on > U+007F).
BEZEL_TEST(encoding_getbytecount_span_1355, ascii_getbytes_span_ok) {
    // per stdlib §3.2 v0.165 (#1355)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var dst := new Array<byte>(4, 0); "
        "  var n := Encoding.ASCII.GetBytes(\"AB\", dst.AsSpan()); "
        "  if n <> 2 then return 1; end; "
        "  if int(dst[0]) <> 65 then return 2; end; "  // 'A'
        "  if int(dst[1]) <> 66 then return 3; end; "  // 'B'
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── GetString(bytes, index, count): window over Count (§3.2 v0.165) ───────────

BEZEL_TEST(encoding_getbytecount_span_1355, getstring_window_selects_range) {
    // per stdlib §3.2 v0.165 (#1355): bytes "Hi!" → decode [1,2) = "i!".
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(0, 0); "
        "  b.Add(byte(72)); b.Add(byte(105)); b.Add(byte(33)); "  // 'H','i','!'
        "  using var s := Encoding.UTF8.GetString(b, 1, 2); "
        "  return s.Length; "  // "i!" → 2
        "end Main; end C;"), 2);
}

BEZEL_TEST(encoding_getbytecount_span_1355, getstring_window_out_of_range_raises) {
    // per stdlib §3.2 v0.165 (#1355): a window past Count → IndexOutOfRangeException.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(0, 0); "
        "  b.Add(byte(72)); b.Add(byte(105)); "  // Count = 2
        "  try "
        "    using var s := Encoding.UTF8.GetString(b, 1, 5); "  // 1+5 > 2
        "    return 0; "
        "  catch IndexOutOfRangeException => return 9; "
        "  end; "
        "end Main; end C;"), 9);
}
