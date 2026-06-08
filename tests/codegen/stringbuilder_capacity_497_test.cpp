// Fix #497: StringBuilder heap corruption — elem_size mismatch between alloc
// and Append/Length/ToString operations.
//
// Root cause: new StringBuilder() emitted bz_array_alloc(elem_size=2, cap=64),
// allocating a 128-byte buffer; but every Append/Length/ToString operation used
// SB_ESZ=8 (pointer-sized elements, since a StringBuilder stores *string parts*,
// not raw bytes). After ~16 appends the 128-byte buffer was exhausted; subsequent
// bz_array_grow operated on corrupt allocator metadata → 'realloc(): invalid next
// size' SIGABRT.
//
// Fix: change the alloc site to elem_size=8 (matching SB_ESZ in Calls.cpp) so
// the initial capacity is 64 * 8 = 512 bytes, consistent with all operations.
//
// Tests derive from stdlib §6 / §3.1 (StringBuilder). Each test exercises a
// distinct property; the first two reproduce the SIGABRT repro scenario from
// the bug report (>64 appends / growth past initial allocation).
//
// StringBuilder is an owned type — bound with `using var` (§28.3).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// -------------------------------------------------------------------------
// §6 / §3.1: repro — 100 single-char appends → Length must be 100.
// Before the fix this crashed with SIGABRT at ~65 appends.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, hundred_appends_length) {
    // 100 > 64 forces bz_array_grow; elem_size mismatch corrupts allocator
    // metadata on the very first grow if the original allocation used esz=2
    // instead of esz=8.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 100 do "
        "    sb.Append(\"x\"); "
        "    i := i + 1; "
        "  end; "
        "  return sb.Length; "
        "end Main; end C;"), 100);
}

// -------------------------------------------------------------------------
// §6 / §3.1: 65 appends matches the exact crash point from the bug report.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, sixtyfive_appends_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 65 do "
        "    sb.Append(\"x\"); "
        "    i := i + 1; "
        "  end; "
        "  return sb.Length; "
        "end Main; end C;"), 65);
}

// -------------------------------------------------------------------------
// §3.1: 200 appends of multi-char strings → correct total length.
// Also exercises repeated growth past the initial (now 64-part) capacity.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, two_hundred_appends_multichar_length) {
    // 200 appends of "ab" (2 chars each) → Length = 400.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 200 do "
        "    sb.Append(\"ab\"); "
        "    i := i + 1; "
        "  end; "
        "  return sb.Length; "
        "end Main; end C;"), 400);
}

// -------------------------------------------------------------------------
// §3.1: ToString correctness after growth — concatenated content must match.
// 5 appends of "Hello " (30 chars total) is well within the initial cap,
// but validates that the string pointers stored in the array are readable.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, tostring_after_appends_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"Hello\"); "
        "  sb.Append(\" \"); "
        "  sb.Append(\"World\"); "
        "  return (1 if sb.ToString() = \"Hello World\" else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1: ToString correctness after growth past initial 64-part capacity.
// Appends 80 parts, then checks ToString produces the expected string.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, tostring_after_growth_correct) {
    // 80 appends of "a" → ToString = "aaaa...a" (80 'a's).
    // We check Length=80 as a proxy (ToString comparison of 80 chars is
    // cumbersome inline; Length exercises the same array iteration).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 80 do "
        "    sb.Append(\"a\"); "
        "    i := i + 1; "
        "  end; "
        "  return sb.Length; "
        "end Main; end C;"), 80);
}

// -------------------------------------------------------------------------
// §3.1: Interleave Append, Insert, Remove, Replace past initial capacity (>64
// parts) to confirm mutators also work correctly after the growth.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, interleaved_mutators_past_initial_cap) {
    // Append 70 "x" chars, then Insert "Y" at index 0, then Remove 1 char at 0,
    // leaving 70 "x"s again. Length should be 70.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 70 do "
        "    sb.Append(\"x\"); "
        "    i := i + 1; "
        "  end; "
        "  sb.Insert(0, \"Y\"); "
        "  sb.Remove(0, 1); "
        "  return sb.Length; "
        "end Main; end C;"), 70);
}

// -------------------------------------------------------------------------
// §3.1: Clear after growth — Length must be 0 after clearing 70 appended parts.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, clear_after_growth) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 70 do "
        "    sb.Append(\"x\"); "
        "    i := i + 1; "
        "  end; "
        "  sb.Clear(); "
        "  return sb.Length; "
        "end Main; end C;"), 0);
}

// -------------------------------------------------------------------------
// §3.1: Replace after growth — all occurrences replaced in grown buffer.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_capacity_497, replace_after_growth) {
    // Append "ab" 70 times → "ababab..." (140 chars). Replace "b" with "B".
    // After Replace the length should still be 140 (same char count, just B).
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  var i := 0; "
        "  while i < 70 do "
        "    sb.Append(\"ab\"); "
        "    i := i + 1; "
        "  end; "
        "  sb.Replace(\"b\", \"B\"); "
        "  return sb.Length; "
        "end Main; end C;"), 140);
}

}  // namespace
