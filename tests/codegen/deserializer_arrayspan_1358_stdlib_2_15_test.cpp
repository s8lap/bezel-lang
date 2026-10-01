// Spec-derived tests for fix #1358 — the additive Array<byte> ↔ span bridges.
//
// Authority: stdlib v0.165 (#1358):
//   * §2.15 — JsonDeserializer / BinaryDeserializer gain a
//       `constructor(input: ReadOnlyArraySpan<byte>)` twin (borrowed identically
//       to the existing ReadOnlySpan<byte> ctor), so
//       `File.ReadAllBytes(p).AsReadOnlySpan()` feeds a deserializer directly.
//   * §7.3 — `File.WriteAllBytes(path, contents: ReadOnlySpan<byte>)` so a
//       serializer's byte output reaches a file without an element copy.
//   * §4.15 — the byte-buffer accessors mirrored onto the Array-span family,
//       closing the file→binary-parse path.
//
// The deserializer read machinery (BeginObject/ReadInt over the wire format) is
// exercised elsewhere; these tests pin the #1358 bridge surface: the span ctors
// are accepted and construct/borrow/dispose cleanly, File.WriteAllBytes(span)
// round-trips, and the byte-accessor + File composition works end-to-end.
//
// Bezel syntax: inequality `<>` (§17.3); byte value `byte(N)`.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/test_data_paths.hpp"  // #5230: per-process @DATA@ paths
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(bezel::test::spliceDataPaths(src, "bz1358"));
    return r.ran ? r.exitCode : -999;
}

// A compile-time reject — doesn't run, so the stricter AOT front end
// (ownership/DA/§33.5 entry-point checks runJit skips) is the correct oracle.
bool rejects(const std::string& src) {
    auto r = runAot(bezel::test::spliceDataPaths(src, "bz1358"));
    return !r.ran && r.diagnostics.hasErrors();
}

}  // namespace

// ── §7.3: File.WriteAllBytes(path, ReadOnlySpan<byte>) round-trips ────────────

BEZEL_TEST(deserializer_arrayspan_1358, file_writeallbytes_span_roundtrip) {
    // per stdlib §7.3 v0.165 (#1358): write a span, read the bytes back.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.IO; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(3, 0); "
        "  b[0] := byte(7); b[1] := byte(8); b[2] := byte(9); "
        "  File.WriteAllBytes(\"@DATA@/bz_1358_rt1.bin\", b.AsReadOnlySpan()); "
        "  using var back := File.ReadAllBytes(\"@DATA@/bz_1358_rt1.bin\"); "
        "  if back.Count <> 3 then return 1; end; "
        "  if int(back[0]) <> 7 then return 2; end; "
        "  if int(back[2]) <> 9 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// A sliced span writes only the selected window.
BEZEL_TEST(deserializer_arrayspan_1358, file_writeallbytes_sliced_span) {
    // per stdlib §7.3/§4.14 v0.165 (#1358): the span carries its own range.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.IO; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b[0] := byte(1); b[1] := byte(2); b[2] := byte(3); b[3] := byte(4); "
        "  File.WriteAllBytes(\"@DATA@/bz_1358_rt2.bin\", b.AsReadOnlySpan().Slice(1, 2)); "
        "  using var back := File.ReadAllBytes(\"@DATA@/bz_1358_rt2.bin\"); "
        "  if back.Count <> 2 then return 1; end; "
        "  if int(back[0]) <> 2 then return 2; end; "
        "  if int(back[1]) <> 3 then return 3; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// The Array<byte> overload of WriteAllBytes remains available (not removed).
BEZEL_TEST(deserializer_arrayspan_1358, file_writeallbytes_array_still_works) {
    // per stdlib §7.3 (#1358): additive — the pre-existing Array form stays.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.IO; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(11); b[1] := byte(22); "
        "  File.WriteAllBytes(\"@DATA@/bz_1358_rt3.bin\", b); "
        "  using var back := File.ReadAllBytes(\"@DATA@/bz_1358_rt3.bin\"); "
        "  return int(back[0]) + int(back[1]); "
        "end Main; end C;"), 33);
}

// ── §2.15: deserializer constructed from a ReadOnlyArraySpan<byte> ────────────

// BinaryDeserializer(ReadOnlyArraySpan<byte>) — constructs, borrows, disposes.
BEZEL_TEST(deserializer_arrayspan_1358, binary_deserializer_arrayspan_ctor) {
    // per stdlib §2.15 v0.165 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// JsonDeserializer(ReadOnlyArraySpan<byte>) — constructs, borrows, disposes.
BEZEL_TEST(deserializer_arrayspan_1358, json_deserializer_arrayspan_ctor) {
    // per stdlib §2.15 v0.165 (#1358)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(2, 0); "
        "  b[0] := byte(52); b[1] := byte(50); "  // \"42\"
        "  using var d := new JsonDeserializer(b.AsReadOnlySpan()); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// The file→deserialize bridge form of §1358: ReadAllBytes → AsReadOnlySpan → ctor.
BEZEL_TEST(deserializer_arrayspan_1358, file_readallbytes_to_deserializer) {
    // per stdlib §2.15/§7.3 v0.165 (#1358): zero-copy file→deserializer path.
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; import System.IO; "
        "import System.Serialization; "
        "class C public static method Main(): int "
        "  using var src := new Array<byte>(4, 0); "
        "  File.WriteAllBytes(\"@DATA@/bz_1358_rt4.bin\", src.AsReadOnlySpan()); "
        "  using var bytes := File.ReadAllBytes(\"@DATA@/bz_1358_rt4.bin\"); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  return 0; "
        "end Main; end C;"), 0);
}

// An OwningBuffer<byte> arg to the deserializer is still rejected (§2.15/#631):
// the ctor takes a span, not the owning buffer — call .AsReadOnlySpan() first.
BEZEL_TEST(deserializer_arrayspan_1358, binary_deserializer_owned_buffer_rejected) {
    // per stdlib §2.15 (#631) — unchanged by the #1358 additive twin.
    BEZEL_EXPECT_TRUE(rejects(
        "module M; import System.Collections.Generic; import System.Serialization; "
        "class C public static method Main(): int "
        "  using var ob := new OwningBuffer<byte>(4); "
        "  using var d := new BinaryDeserializer(ob); "
        "  return 0; "
        "end Main; end C;"));
}

// ── §4.15 composition: file → byte-accessor parse (the #1358 headline path) ───

BEZEL_TEST(deserializer_arrayspan_1358, file_then_byte_accessor_parse) {
    // per stdlib §4.15/§7.3 v0.165 (#1358): write a LE int32 into a byte array,
    // persist it, read it back, and parse it with the mirrored byte accessor —
    // `File.ReadAllBytes(p).AsReadOnlySpan().ReadUInt32LittleEndian()`.
    BEZEL_EXPECT_EQ(runAotOut(bezel::test::spliceDataPaths(
        "module M; import System.Collections.Generic; import System.IO; "
        "class C public static method Main(): int "
        "  using var b := new Array<byte>(4, 0); "
        "  b.AsSpan().WriteInt32LittleEndian(0x12345678); "
        "  File.WriteAllBytes(\"@DATA@/bz_1358_rt5.bin\", b.AsReadOnlySpan()); "
        "  using var back := File.ReadAllBytes(\"@DATA@/bz_1358_rt5.bin\"); "
        "  Console.WriteLine(back.AsReadOnlySpan().ReadInt32LittleEndian()); return 0; "
        "end Main; end C;", "bz1358")), "305419896");
}
