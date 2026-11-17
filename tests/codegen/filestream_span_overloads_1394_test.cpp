// fix #1394 (stdlib §7.1 `Stream` span overloads, v0.357; §7.2 `FileStream`).
//
// stdlib §7.1 declares, on `Stream` (and so on `FileStream`):
//
//     public method Read(buffer: Span<byte>): int;        // span carries its own range
//     public method Write(buffer: ReadOnlySpan<byte>);    //   (slice first: §1, Window parameters)
//
// and the "Read/write contracts" paragraph gives `Read` the same 0-iff-EOF /
// short-read contract over the span's own `Length`, and `Write` the
// whole-window rule. Before the fix NEITHER form had a lowering arm, so the
// ergonomics study's `fs.Write(buf.AsReadOnlySpan())` (study item B15) died at
// compile time with
//
//   `Write` on built-in type `FileStream` produced no value, and no dispatch
//   arm emitted any code for this call. ... (#2457)
//
// — which also cited the CLOSED #2457 rather than a live tracker. Every
// runtime row below therefore has `ran == false` (-999) without the fix.
//
// Already covered elsewhere, and NOT repeated here: the Count-window guards
// of the `(buffer, offset, count)` forms (§7.1 "Windowed-form contract", the
// IndexOutOfRangeException rows of filestream_645_test.cpp). A span carries
// its own range, so the span forms have no second window to guard.
//
// Left unimplemented by this fix (tracked on #1394, `narrow`): the `FileStream`
// async members (`ReadAsync`/`WriteAsync`/`ReadAllAsync`, §7.1/§7.2) and any
// `Stream`-typed receiver. The last row pins that the diagnostic they still
// draw cites #1394.
//
// Paths are unique per test (@DATA@/bzt1394_*, a per-process dir, #5230).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& program) {
    auto r = runAotChecked(bezel::test::spliceDataPaths(program, "bzt1394"));
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// True when the program is REJECTED at compile time with an error whose text
// contains `needle` — the specific diagnostic, never merely "some error".
bool rejectedWith(const std::string& program, const std::string& needle) {
    auto r = runAot(bezel::test::spliceDataPaths(program, "bzt1394"));
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §7.1: the study's literal repro, closed into a round trip. A span Write of a
// four-byte OwningBuffer, then a span Read of the same file into a larger
// buffer: `Read` returns the 4 bytes available (a short read, not a failure),
// the value read back is the value written, and the next `Read` returns 0 —
// "0 iff the stream is at end".
BEZEL_TEST(filestream_span_1394_7_1, span_write_then_span_read_round_trips_an_int) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var buf := new OwningBuffer<byte>(4); "
        "buf.AsSpan().WriteInt32LittleEndian(305419896); "
        "using var w := new FileStream(\"@DATA@/bzt1394_rt.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(buf.AsReadOnlySpan()); "
        "w.Dispose(); "
        "using var dst := new OwningBuffer<byte>(8); "
        "using var r := new FileStream(\"@DATA@/bzt1394_rt.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var n := r.Read(dst.AsSpan()); "
        "var v := dst.AsReadOnlySpan().ReadInt32LittleEndian(); "
        "var again := r.Read(dst.AsSpan()); "
        "if n = 4 and v = 305419896 and again = 0 then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 "the span carries its own range": a `Write` of `a.AsReadOnlySpan(2, 3)`
// writes exactly the three bytes of that window, and a following `Write` of
// `a.AsSpan(0, 2)` (a WRITABLE span, which converts to its read-only twin)
// appends exactly its two. The file is [3,4,5,1,2]: Length 5.
BEZEL_TEST(filestream_span_1394_7_1, span_write_writes_exactly_the_spans_window) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var a := new Array<byte>(6, byte(0)); "
        "a[0] := byte(1); a[1] := byte(2); a[2] := byte(3); "
        "a[3] := byte(4); a[4] := byte(5); a[5] := byte(6); "
        "using var f := new FileStream(\"@DATA@/bzt1394_win.bin\", "
        "                             FileMode.Create, FileAccess.ReadWrite); "
        "f.Write(a.AsReadOnlySpan(2, 3)); "
        "f.Write(a.AsSpan(0, 2)); "
        "using var b := new Array<byte>(8, byte(0)); "
        "f.Position := 0L; "
        "var n := f.Read(b, 0, 8); "
        "if f.Length = 5L and n = 5 and b[0] = byte(3) and b[1] = byte(4) "
        "   and b[2] = byte(5) and b[3] = byte(1) and b[4] = byte(2) "
        "   and b[5] = byte(0) then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 `Read(buffer: Span<byte>)` over a SLICE: `b.AsSpan(1, 4)` is the whole
// window, so exactly its four bytes are filled, `Read` returns 4, and the
// elements outside the slice (b[0], b[5]) are untouched ("writes into existing
// elements"). The five-byte file is [3,4,5,1,2], so b = [0,3,4,5,1,0,...].
BEZEL_TEST(filestream_span_1394_7_1, span_read_fills_only_the_slice) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var a := new Array<byte>(5, byte(0)); "
        "a[0] := byte(3); a[1] := byte(4); a[2] := byte(5); "
        "a[3] := byte(1); a[4] := byte(2); "
        "using var f := new FileStream(\"@DATA@/bzt1394_slice.bin\", "
        "                             FileMode.Create, FileAccess.ReadWrite); "
        "f.Write(a, 0, 5); "
        "f.Position := 0L; "
        "using var b := new Array<byte>(8, byte(0)); "
        "var n := f.Read(b.AsSpan(1, 4)); "
        "if n = 4 and b[0] = byte(0) and b[1] = byte(3) and b[2] = byte(4) "
        "   and b[3] = byte(5) and b[4] = byte(1) and b[5] = byte(0) "
        "   then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 "a zero-length window returns 0 immediately, the one carve-out", over
// the span's own `Length`: an EMPTY span returns 0 even though data remains
// (the stream is not at end), and the position does not move.
BEZEL_TEST(filestream_span_1394_7_1, span_read_of_an_empty_span_returns_zero_without_consuming) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var a := new Array<byte>(2, byte(9)); "
        "using var f := new FileStream(\"@DATA@/bzt1394_empty.bin\", "
        "                             FileMode.Create, FileAccess.ReadWrite); "
        "f.Write(a.AsReadOnlySpan()); "
        "f.Position := 0L; "
        "using var b := new Array<byte>(2, byte(0)); "
        "var z := f.Read(b.AsSpan(0, 0)); "
        "var n := f.Read(b.AsSpan()); "
        "if z = 0 and n = 2 and b[0] = byte(9) and b[1] = byte(9) "
        "   then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 `Read(buffer: Span<byte>)` takes a WRITABLE span. A `ReadOnlySpan<byte>`
// would let `Read` write through a read-only view, so it is rejected — with
// its own diagnostic, not the generic "no dispatch arm" backstop.
BEZEL_TEST(filestream_span_1394_7_1, span_read_rejects_a_read_only_span) {
    BEZEL_EXPECT_TRUE(rejectedWith(prog(
        "using var buf := new OwningBuffer<byte>(4); "
        "using var r := new FileStream(\"@DATA@/bzt1394_ro.bin\", "
        "                             FileMode.OpenOrCreate, FileAccess.ReadWrite); "
        "return r.Read(buf.AsReadOnlySpan()); "),
        "`FileStream.Read` takes a writable `Span<byte>`"));
}

// §7.1 declares BYTE spans. A `ReadOnlyArraySpan<int>` handed to `Write` would
// have its `int` storage written as if it were bytes (start/length in
// elements, the runtime copying bytes), so it is rejected.
BEZEL_TEST(filestream_span_1394_7_1, span_write_rejects_a_non_byte_span) {
    BEZEL_EXPECT_TRUE(rejectedWith(prog(
        "using var a := new Array<int>(4, 0); "
        "using var w := new FileStream(\"@DATA@/bzt1394_int.bin\", "
        "                             FileMode.Create, FileAccess.Write); "
        "w.Write(a.AsReadOnlySpan()); "
        "return 0; "),
        "`FileStream.Write` takes a `ReadOnlySpan<byte>`"));
}

// A `FileStream` member that is still unimplemented (the async surface,
// §7.1/§7.2) draws the "no dispatch arm" diagnostic, and that diagnostic now
// cites the LIVE tracker #1394 rather than the closed #2457.
BEZEL_TEST(filestream_span_1394_7_1, unimplemented_filestream_member_cites_the_live_tracker) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C "
        "  public static async method F(path: string): Task<int> "
        "    using var a := new Array<byte>(4, byte(0)); "
        "    using var r := new FileStream(path, FileMode.Create, FileAccess.ReadWrite); "
        "    await r.WriteAsync(a, 0, 4); "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; end C;",
        "not implemented by this compiler (#1394)"));
}
