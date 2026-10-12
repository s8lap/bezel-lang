// Spec-derived tests for fix #1358 — endianness-aware byte-buffer accessors on
// the four span-over-byte types.
//
// Authority: stdlib §4.14 (the extension family on Span<byte>/ReadOnlySpan<byte>)
// and §4.15 v0.165 (#1358) which mirrors the same family onto ArraySpan<byte> /
// ReadOnlyArraySpan<byte>:
//   Read/Write{Int16,UInt16,Int32,UInt32,Int64,UInt64,Float,Double}{Little,Big}Endian
//   and their Try* forms. Reads and writes operate from the START of the span —
//   "No offset parameter ... To read at an offset, slice first" (§4.14). Read
//   methods throw IndexOutOfRangeException on a too-short span; TryRead returns
//   false and assigns default(T); TryWrite returns false and leaves the span
//   unmodified.
//
// Bezel syntax: inequality `<>` (§17.3); byte value `byte(N)`; the `out`
// parameter of a TryRead uses `out var v` (§22.4). Raises are pinned in-source.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Read: 16-bit LE/BE across ReadOnlyArraySpan ───────────────────────────────

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int16_le) {
    // per stdlib §4.15 v0.165 (#1358): 0x01 0x00 → LE i16 = 1.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(1); b[1] := byte(0); "
        "  return int(b.AsReadOnlySpan().ReadInt16LittleEndian()); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int16_le_256) {
    // per stdlib §4.15 v0.165 (#1358): 0x00 0x01 → LE i16 = 256.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(0); b[1] := byte(1); "
        "  Console.WriteLine(int(b.AsReadOnlySpan().ReadInt16LittleEndian())); return 0; "
        "end Main; end C;"), "256");
}

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int16_be) {
    // per stdlib §4.15 v0.165 (#1358): 0x00 0x01 → BE i16 = 1.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(0); b[1] := byte(1); "
        "  return int(b.AsReadOnlySpan().ReadInt16BigEndian()); "
        "end Main; end C;"), 1);
}

// ── Read: 32-bit LE/BE, 0x12345678, across all four span families ─────────────

// ReadOnlyArraySpan (Array.AsReadOnlySpan) — the §4.15 file→parse path.
BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_le_readonly_arrayspan) {
    // per stdlib §4.15 v0.165 (#1358): 0x78 0x56 0x34 0x12 → LE 0x12345678.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(120); b[1] := byte(86); b[2] := byte(52); b[3] := byte(18); "
        "  Console.WriteLine(b.AsReadOnlySpan().ReadInt32LittleEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

// ArraySpan (Array.AsSpan, writable form).
BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_le_arrayspan) {
    // per stdlib §4.15 v0.165 (#1358)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(120); b[1] := byte(86); b[2] := byte(52); b[3] := byte(18); "
        "  Console.WriteLine(b.AsSpan().ReadInt32LittleEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

// Span (OwningBuffer.AsSpan) — the §4.14 owning-buffer family.
BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_le_span) {
    // per stdlib §4.14 (#1358)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ob := new OwningBuffer<byte>(4); "
        "  var sp := ob.AsSpan(); "
        "  sp[0] := byte(120); sp[1] := byte(86); sp[2] := byte(52); sp[3] := byte(18); "
        "  Console.WriteLine(sp.ReadInt32LittleEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

// ReadOnlySpan (OwningBuffer.AsReadOnlySpan).
BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_le_readonly_span) {
    // per stdlib §4.14 (#1358)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ob := new OwningBuffer<byte>(4); "
        "  var w := ob.AsSpan(); "
        "  w[0] := byte(120); w[1] := byte(86); w[2] := byte(52); w[3] := byte(18); "
        "  Console.WriteLine(ob.AsReadOnlySpan().ReadInt32LittleEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_be) {
    // per stdlib §4.15 v0.165 (#1358): 0x12 0x34 0x56 0x78 → BE 0x12345678.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(18); b[1] := byte(52); b[2] := byte(86); b[3] := byte(120); "
        "  Console.WriteLine(b.AsReadOnlySpan().ReadInt32BigEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

// ── Read: 64-bit and unsigned ─────────────────────────────────────────────────

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int64_le) {
    // per stdlib §4.15 v0.165 (#1358): low byte 0x2A, rest 0 → 42.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(8, 0); "
        "  b[0] := byte(42); "
        "  return int(b.AsReadOnlySpan().ReadInt64LittleEndian()); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(arrayspan_byte_accessors_1358, read_uint16_le) {
    // per stdlib §4.15 v0.165 (#1358): 0xFF 0xFF → uint16 65535.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(255); b[1] := byte(255); "
        "  Console.WriteLine(int(b.AsReadOnlySpan().ReadUInt16LittleEndian())); return 0; "
        "end Main; end C;"), "65535");
}

// ── Write (ArraySpan / Span) round-trips through Read ─────────────────────────

BEZEL_TEST(arrayspan_byte_accessors_1358, round_trip_int32_le_arrayspan) {
    // per stdlib §4.15 v0.165 (#1358)
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  var sp := b.AsSpan(); "
        "  sp.WriteInt32LittleEndian(0x12345678); "
        "  Console.WriteLine(sp.ReadInt32LittleEndian()); return 0; "
        "end Main; end C;"), "305419896");
}

BEZEL_TEST(arrayspan_byte_accessors_1358, round_trip_int32_be_span) {
    // per stdlib §4.14 (#1358): write BE, read BE.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ob := new OwningBuffer<byte>(4); "
        "  var sp := ob.AsSpan(); "
        "  sp.WriteInt32BigEndian(0x0ABCDE01); "
        "  Console.WriteLine(sp.ReadInt32BigEndian()); return 0; "
        "end Main; end C;"), "180149761");
}

// BE vs LE actually differ: write LE, read BE ⇒ byte-swapped value.
BEZEL_TEST(arrayspan_byte_accessors_1358, endianness_distinguished) {
    // per stdlib §4.15 v0.165 (#1358): 0x00000001 LE reads as 0x01000000 BE.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  var sp := b.AsSpan(); "
        "  sp.WriteInt32LittleEndian(1); "
        "  Console.WriteLine(sp.ReadInt32BigEndian()); return 0; "  // 0x01000000
        "end Main; end C;"), "16777216");
}

// Float / double round-trip (bit-pattern preserving, §4.14 "Float bit handling").
BEZEL_TEST(arrayspan_byte_accessors_1358, round_trip_float_le) {
    // per stdlib §4.14 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  var sp := b.AsSpan(); "
        "  sp.WriteFloatLittleEndian(1.5f); "
        "  if sp.ReadFloatLittleEndian() <> 1.5f then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(arrayspan_byte_accessors_1358, round_trip_double_be) {
    // per stdlib §4.14 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var ob := new OwningBuffer<byte>(8); "
        "  var sp := ob.AsSpan(); "
        "  sp.WriteDoubleBigEndian(3.25); "
        "  if sp.ReadDoubleBigEndian() <> 3.25 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── "slice first" for offset reads (§4.14) ────────────────────────────────────

BEZEL_TEST(arrayspan_byte_accessors_1358, slice_then_read_offset) {
    // per stdlib §4.14/§4.15 (#1358): read the i16 at offset 2 via Slice(2, 2).
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(0); b[1] := byte(0); b[2] := byte(5); b[3] := byte(0); "
        "  return int(b.AsReadOnlySpan().Slice(2, 2).ReadInt16LittleEndian()); "
        "end Main; end C;"), 5);
}

// ── Try* variants ─────────────────────────────────────────────────────────────

BEZEL_TEST(arrayspan_byte_accessors_1358, try_read_int32_le_success) {
    // per stdlib §4.14/§4.15 (#1358): enough room → true, out = value.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(120); b[1] := byte(86); b[2] := byte(52); b[3] := byte(18); "
        "  if b.AsReadOnlySpan().TryReadInt32LittleEndian(out var v) then "
        "    Console.WriteLine(v); return 0; "
        "  end; "
        "  return -1; "
        "end Main; end C;"), "305419896");
}

BEZEL_TEST(arrayspan_byte_accessors_1358, try_read_int32_le_too_short_false) {
    // per stdlib §4.14/§4.15 (#1358): short span → false, out = default(0).
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(3, 0); "  // only 3 bytes
        "  if b.AsReadOnlySpan().TryReadInt32LittleEndian(out var v) then "
        "    return 1; "  // must not succeed
        "  end; "
        "  if v <> 0 then return 2; end; "  // default assigned on false path
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(arrayspan_byte_accessors_1358, try_write_int32_le_success) {
    // per stdlib §4.15 v0.165 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  var sp := b.AsSpan(); "
        "  if not sp.TryWriteInt32LittleEndian(1) then return 1; end; "
        "  if int(b[0]) <> 1 then return 2; end; "
        "  if int(b[1]) <> 0 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(arrayspan_byte_accessors_1358, try_write_int32_le_too_short_unmodified) {
    // per stdlib §4.14/§4.15 (#1358): "On the false path the span is not modified".
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, byte(99)); "  // too short for a 4-byte write
        "  var sp := b.AsSpan(); "
        "  if sp.TryWriteInt32LittleEndian(12345) then return 1; end; "  // must be false
        "  if int(b[0]) <> 99 then return 2; end; "  // unchanged
        "  if int(b[1]) <> 99 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ── Non-Try read on a too-short span raises IndexOutOfRangeException ──────────

BEZEL_TEST(arrayspan_byte_accessors_1358, read_int32_too_short_raises) {
    // per stdlib §4.14/§4.15 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(3, 0); "
        "  try "
        "    var v := b.AsReadOnlySpan().ReadInt32LittleEndian(); "
        "    return 0; "
        "  catch IndexOutOfRangeException => return 9; "
        "  end; "
        "end Main; end C;"), 9);
}

// ── Writes are rejected on a read-only span at compile time (§4.15) ──────────

BEZEL_TEST(arrayspan_byte_accessors_1358, write_on_readonly_span_rejected) {
    // per stdlib §4.15 v0.165 (#1358): Write* requires a writable span.
    auto r = runAot(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b.AsReadOnlySpan().WriteInt32LittleEndian(1); "  // read-only → error
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!r.ran && r.diagnostics.hasErrors());
}
