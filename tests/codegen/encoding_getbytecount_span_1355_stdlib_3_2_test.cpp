// Spec-derived tests for fix #1355 — Encoding.GetByteCount and
// Encoding.GetBytes(s, destination: Span<byte>), the two new overloads added in
// stdlib §3.2 v0.165.
//
// Authority: stdlib §3.2 v0.165:
//   public abstract method GetByteCount(s: string): int;
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//       // returns bytes written; IndexOutOfRangeException if destination too short
// Error model (v0.136, unchanged): malformed/unrepresentable input is a defined
// fault — ASCII.GetByteCount/GetBytes raise FormatException on any code point
// above U+007F.
//
// fix #3851 (stdlib §3.2 / §1, v0.290, proposal 0123 R5 Option B): this file
// originally also carried the `GetString(bytes, index, count)` window
// contract added alongside #1355 — v0.290 retired that three-argument call
// form outright (a window over an Array<byte> is a slice, not a parameter
// pair; §4.14's no-offset-parameter rule). The two rows below now pin the
// REJECTION instead: `getstring_window_selects_range` (a successful decode)
// and `getstring_window_out_of_range_raises` (its runtime
// IndexOutOfRangeException bounds check) can no longer be expressed at all
// through this call form, so both become compile-rejection rows.
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

// fix #3851: true iff some diagnostic contains `sub`.
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// fix #3851: the diagnostic must point at the call, never at 0:0 (mirrors
// tests/codegen/encoding_test.cpp's `hasErrorLocatedAt` / tests/sema/
// string_isnullorempty_removed_3854_test.cpp's `locatedAt`). Parses the real
// `<file>:<line>:<col>:` prefix the driver renders ahead of `needle`.
bool hasErrorLocatedAt(const bezel::test::JitResult& r,
                       const std::string& needle) {
    const std::string tail = ": " + needle;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        for (std::size_t at = m.find(tail); at != std::string::npos;
             at = m.find(tail, at + 1)) {
            std::size_t p = at;
            auto digitsBack = [&m](std::size_t& q) -> long {
                const std::size_t end = q;
                while (q > 0 && m[q - 1] >= '0' && m[q - 1] <= '9') --q;
                if (q == end) return -1;
                return std::stol(m.substr(q, end - q));
            };
            const long col = digitsBack(p);
            if (col <= 0 || p == 0 || m[p - 1] != ':') continue;
            --p;
            if (digitsBack(p) > 0) return true;
        }
    }
    return false;
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

// ── GetString(bytes, index, count): the retired window (fix #3851) ────────────
//
// v0.290 (proposal 0123 R5 Option B) retired the three-argument call form
// these two rows used to exercise. Both are now REJECTION rows: the window
// they used to select, and the runtime out-of-range bounds check its guard
// used to raise, can no longer be reached through this call form at all —
// the call is rejected in sema before codegen (or any runtime guard) is ever
// reached.

BEZEL_TEST(encoding_getbytecount_span_1355,
           getstring_window_call_form_retired_3851_in_range) {
    // per stdlib §3.2 v0.290 (fix #3851): the in-range window call
    // `GetString(b, 1, 2)` this row used to accept and decode ("Hi!" →
    // [1,2) = "i!") is now rejected outright.
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(0, 0); "
        "  b.Add(byte(72)); b.Add(byte(105)); b.Add(byte(33)); "  // 'H','i','!'
        "  using var s := Encoding.UTF8.GetString(b, 1, 2); "
        "  return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "was REMOVED from the standard library at v0.290"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "GetString(bytes.AsReadOnlySpan(start, length))"));
    BEZEL_EXPECT_TRUE(hasErrorLocatedAt(
        r, "the three-argument `Encoding.GetString(bytes, start, length)`"));
}

BEZEL_TEST(encoding_getbytecount_span_1355,
           getstring_window_call_form_retired_3851_out_of_range) {
    // per stdlib §3.2 v0.290 (fix #3851): the out-of-range window call
    // `GetString(b, 1, 5)` (1+5 > Count=2) this row used to accept and then
    // raise IndexOutOfRangeException at runtime is now rejected at compile
    // time instead — the same diagnostic as the in-range control above,
    // since the call form itself is gone regardless of the window's bounds.
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(0, 0); "
        "  b.Add(byte(72)); b.Add(byte(105)); "  // Count = 2
        "  try "
        "    using var s := Encoding.UTF8.GetString(b, 1, 5); "  // 1+5 > 2
        "    return 0; "
        "  catch IndexOutOfRangeException => return 9; "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "was REMOVED from the standard library at v0.290"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "GetString(bytes.AsReadOnlySpan(start, length))"));
}
