// Codegen tests for stdlib §7.2 `FileStream` and the §7.1/§7.2 System.IO enums
// (`FileMode`, `FileAccess`, `SeekOrigin`) — fix #645.
//
// Root cause (fix #645): NONE of these names existed in the compiler. Before
// the fix every one of the programs below failed to COMPILE:
//
//   probe.bzl:0:0: unresolved type name `FileStream`
//   probe.bzl:5:75: unresolved identifier `FileMode`
//   probe.bzl:5:92: unresolved identifier `FileAccess`
//
// so `ran` is false and `run()` yields -999 rather than the expected exit code
// — every test here fails without the fix and passes with it. (A compiler CRASH
// would also produce -999, but these are runtime-VALUE assertions with distinct
// non-zero expected codes, so a crash cannot be mistaken for a pass; #2254.)
//
// The fix has two halves, matching the two halves of the stdlib declaration:
//   * FileMode / FileAccess / SeekOrigin are ordinary Bezel enums, injected
//     VERBATIM from §7.1/§7.2 by the stdlib prelude (Driver.cpp) — so §10.2
//     auto-increment fixes their ordinals and every enum rule applies.
//   * FileStream is a compiler-intrinsic heap class (bz_filestream_t*) with the
//     §7.2 constructors, the §7.1 stream operations, and §7.2's flush-on-dispose
//     destructor wired through the `using var` disposal path.
//
// Deferred (not tested here, with reasons):
//   - ReadAsync / WriteAsync / ReadAllAsync (§7.1): the async I/O runtime is not
//     implemented; the same deferral the §7.3 File tests record.
//   - The Read(Span<byte>) / Write(ReadOnlySpan<byte>) span overloads (§7.1).
//   - IOException / FileNotFoundException on an open or I/O failure (§7.5): the
//     §7 runtime returns status codes, not Bezel exceptions (the same deferral
//     recorded in file_io_644_test.cpp). A failed open yields a degenerate
//     handle whose operations are no-ops; `no_crash_on_missing_file` pins that
//     it does not crash, NOT that it raises.
//   - Using FileStream through its `Stream` base (§7.1) — `Stream` is not a
//     compiler-known type; only the concrete FileStream face is wired.
//
// Paths are unique per test (/tmp/bzt645_*) to avoid cross-test interference.

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Leak/UAF-strict variant (ASan + LeakSanitizer). Used for the tests whose
// point is the §7.2 disposal contract. NOTE: these programs must not touch the
// `File` static methods or the `Path` property — both mint +1 strings /
// malloc'd path buffers the compiler does not release today (a PRE-EXISTING
// leak class, reproducible with `var t := File.ReadAllText(p);` alone), which
// would trip LeakSanitizer for a reason unrelated to FileStream.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a statement-list body in a minimal Main that returns an int.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// §7.1 / §7.2 — the three enums resolve at all.
// This is the issue's literal report: `FileMode` / `FileAccess` were
// "unresolved identifier" and `FileStream` "unresolved type name".
// ---------------------------------------------------------------------------

// §7.2 `public enum FileMode: byte  CreateNew, Create, Open, OpenOrCreate,
// Truncate, Append;` — §10.2 auto-increment gives 0..5 in declaration order.
BEZEL_TEST(filestream_645_7_2, file_mode_enum_resolves_with_declared_order) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := int(byte(FileMode.CreateNew)); "
        "var b := int(byte(FileMode.Create)); "
        "var c := int(byte(FileMode.Open)); "
        "var d := int(byte(FileMode.OpenOrCreate)); "
        "var e := int(byte(FileMode.Truncate)); "
        "var f := int(byte(FileMode.Append)); "
        "if a = 0 and b = 1 and c = 2 and d = 3 and e = 4 and f = 5 then "
        "  return 1; end; "
        "return 0; ")), 1);
}

// §7.2 `public enum FileAccess: byte  Read, Write, ReadWrite;`
BEZEL_TEST(filestream_645_7_2, file_access_enum_resolves_with_declared_order) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := int(byte(FileAccess.Read)); "
        "var w := int(byte(FileAccess.Write)); "
        "var rw := int(byte(FileAccess.ReadWrite)); "
        "if r = 0 and w = 1 and rw = 2 then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 `public enum SeekOrigin: byte  Begin, Current, End;`
BEZEL_TEST(filestream_645_7_1, seek_origin_enum_resolves_with_declared_order) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := int(byte(SeekOrigin.Begin)); "
        "var c := int(byte(SeekOrigin.Current)); "
        "var e := int(byte(SeekOrigin.End)); "
        "if b = 0 and c = 1 and e = 2 then return 1; end; "
        "return 0; ")), 1);
}

// §7.2: the enums are ordinary Bezel enums, so a variable can be TYPED by one
// and the §10.6 prefix-omitted `case` match works over it.
BEZEL_TEST(filestream_645_7_2, file_mode_is_an_ordinary_enum_type) {
    BEZEL_EXPECT_EQ(run(prog(
        "var m: FileMode := FileMode.Truncate; "
        "var n := (case m of "
        "  CreateNew => 10, Create => 11, Open => 12, "
        "  OpenOrCreate => 13, Truncate => 14, Append => 15); "
        "return n; ")), 14);
}

// §2.3 auto-import shadowing (the #2047/#2131 rule the `Result` prelude
// already follows): a user top-level type of the same name SHADOWS the
// injected prelude enum — the user's declaration wins and the stdlib one is
// simply unavailable in that module. Two same-named top-level types would
// corrupt name resolution for the whole module, so this pins that the
// System.IO enums are injected under the same guard.
BEZEL_TEST(filestream_645_7_2, user_file_mode_type_shadows_the_prelude_enum) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public enum FileMode: byte Alpha, Beta, Gamma; end FileMode; "
        "class C public static method Main(): int "
        "  var m: FileMode := FileMode.Gamma; "
        "  return int(byte(m)) + 20; "
        "end Main; end C;"), 22);
}

// ---------------------------------------------------------------------------
// §7.2 — construction, §7.1 Write, and the flush-on-dispose contract.
// ---------------------------------------------------------------------------

// §7.2 `constructor(path, mode, access)` + §7.1 `Write(buffer, offset, count)`.
// The bytes reach the file at `using var` scope exit — §7.2 "Disposal
// (flush-on-dispose) … the `using var` idiom guarantees this on every exit
// edge". The write happens in a helper so the scope really does end.
BEZEL_TEST(filestream_645_7_2, write_reaches_the_file_at_using_var_scope_exit) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method W(path: string) "
        "    using var fs := new FileStream(path, FileMode.Create, FileAccess.Write); "
        "    using var b := new Array<byte>(); "
        "    b.Add(byte(10)); b.Add(byte(20)); b.Add(byte(30)); "
        "    fs.Write(b, 0, 3); "
        "  end W; "
        "  public static method Main(): int "
        "    W(\"/tmp/bzt645_flush.bin\"); "
        "    using var back := File.ReadAllBytes(\"/tmp/bzt645_flush.bin\"); "
        "    File.Delete(\"/tmp/bzt645_flush.bin\"); "
        "    if back.Count = 3 and back[1] = byte(20) then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// §7.2 FileMode.Append — "writes go to the end"; the file is NOT truncated.
BEZEL_TEST(filestream_645_7_2, append_mode_extends_an_existing_file) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Put(path: string, v: byte, mode: FileMode) "
        "    using var fs := new FileStream(path, mode, FileAccess.Write); "
        "    using var b := new Array<byte>(); b.Add(v); "
        "    fs.Write(b, 0, 1); "
        "  end Put; "
        "  public static method Main(): int "
        "    Put(\"/tmp/bzt645_app.bin\", byte(1), FileMode.Create); "
        "    Put(\"/tmp/bzt645_app.bin\", byte(2), FileMode.Append); "
        "    using var back := File.ReadAllBytes(\"/tmp/bzt645_app.bin\"); "
        "    File.Delete(\"/tmp/bzt645_app.bin\"); "
        "    if back.Count = 2 and back[0] = byte(1) and back[1] = byte(2) then "
        "      return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// §7.2 FileMode.Create — "create or truncate": a second Create drops the old
// contents rather than appending to them.
BEZEL_TEST(filestream_645_7_2, create_mode_truncates_an_existing_file) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Put(path: string, v: byte) "
        "    using var fs := new FileStream(path, FileMode.Create, FileAccess.Write); "
        "    using var b := new Array<byte>(); b.Add(v); b.Add(v); "
        "    fs.Write(b, 0, 2); "
        "  end Put; "
        "  public static method Main(): int "
        "    Put(\"/tmp/bzt645_trunc.bin\", byte(1)); "
        "    Put(\"/tmp/bzt645_trunc.bin\", byte(9)); "
        "    using var back := File.ReadAllBytes(\"/tmp/bzt645_trunc.bin\"); "
        "    File.Delete(\"/tmp/bzt645_trunc.bin\"); "
        "    if back.Count = 2 and back[0] = byte(9) then return 1; end; "
        "    return 0; "
        "  end Main; end C;"), 1);
}

// §7.2 `constructor(path: string, mode: FileMode)` — the two-argument form.
// RECORDED CHOICE (#645): §7.2 declares this constructor but does not state
// which FileAccess it implies; the implementation uses FileAccess.ReadWrite.
// This test pins that choice so a later spec ruling has to update it visibly.
BEZEL_TEST(filestream_645_7_2, two_argument_constructor_defaults_to_readwrite) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var w := new FileStream(\"/tmp/bzt645_2arg.bin\", FileMode.Create); "
        "w.Dispose(); "
        "using var rw := new FileStream(\"/tmp/bzt645_2arg.bin\", FileMode.Open); "
        "if rw.CanRead and rw.CanWrite then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §7.1 — the windowed read/write contract.
// ---------------------------------------------------------------------------

// §7.1 "Read returns the number of bytes read into the window" and reads into
// EXISTING elements (the buffer is allocated with the §4.4 sized constructor).
BEZEL_TEST(filestream_645_7_1, read_fills_the_window_and_returns_the_count) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(7)); src.Add(byte(8)); src.Add(byte(9)); "
        "using var w := new FileStream(\"/tmp/bzt645_read.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 3); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_read.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "using var buf := new Array<byte>(3, byte(0)); "
        "var n := r.Read(buf, 0, 3); "
        "if n = 3 and buf[0] = byte(7) and buf[2] = byte(9) then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 "Read … returns 0 iff the stream is at end". The second read of a
// fully-consumed 3-byte stream returns 0.
BEZEL_TEST(filestream_645_7_1, read_returns_zero_at_end_of_stream) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); src.Add(byte(1)); "
        "using var w := new FileStream(\"/tmp/bzt645_eof.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 1); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_eof.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "using var buf := new Array<byte>(2, byte(0)); "
        "var first := r.Read(buf, 0, 2); "
        "var second := r.Read(buf, 0, 2); "
        "if first = 1 and second = 0 then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 "a zero-length window returns 0 immediately, the one carve-out".
BEZEL_TEST(filestream_645_7_1, zero_length_window_returns_zero) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); src.Add(byte(1)); src.Add(byte(2)); "
        "using var w := new FileStream(\"/tmp/bzt645_zerow.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 2); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_zerow.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "using var buf := new Array<byte>(2, byte(0)); "
        "if r.Read(buf, 0, 0) = 0 then return 1; end; "
        "return 0; ")), 1);
}

// §7.1 "the window [offset, offset+count) ranges over the buffer's Count …
// out-of-range raises IndexOutOfRangeException". Caught in-program, so the
// distinctive exit code proves the raise happened (not merely that the program
// died).
BEZEL_TEST(filestream_645_7_1, out_of_range_write_window_raises_index_out_of_range) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); src.Add(byte(1)); src.Add(byte(2)); "
        "using var w := new FileStream(\"/tmp/bzt645_oorw.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "try "
        "  w.Write(src, 0, 5); "
        "  return 0; "
        "catch IndexOutOfRangeException => return 42; "
        "end; ")), 42);
}

// §7.1: the same window rule on the READ side.
BEZEL_TEST(filestream_645_7_1, out_of_range_read_window_raises_index_out_of_range) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var w := new FileStream(\"/tmp/bzt645_oorr.bin\", "
        "                              FileMode.Create, FileAccess.ReadWrite); "
        "using var buf := new Array<byte>(2, byte(0)); "
        "try "
        "  var n := w.Read(buf, 1, 4); "
        "  return 0; "
        "catch IndexOutOfRangeException => return 43; "
        "end; ")), 43);
}

// ---------------------------------------------------------------------------
// §7.1 — Seek / Position / Length / CanRead / CanWrite / CanSeek / ReadAll.
// ---------------------------------------------------------------------------

// §7.1 `Seek(offset, origin): long` returns the new position; `Position` is
// settable; `Length` is the file's byte count.
BEZEL_TEST(filestream_645_7_1, seek_position_and_length) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(1)); src.Add(byte(2)); src.Add(byte(3)); src.Add(byte(4)); "
        "using var w := new FileStream(\"/tmp/bzt645_seek.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_seek.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var mask := 0; "
        "if r.Length = 4 then mask := mask + 1; end; "
        "r.Position := 2; "
        "if r.Position = 2 then mask := mask + 2; end; "
        "using var buf := new Array<byte>(2, byte(0)); "
        "var n := r.Read(buf, 0, 2); "
        "if n = 2 and buf[0] = byte(3) and buf[1] = byte(4) then mask := mask + 4; end; "
        "if r.Seek(1, SeekOrigin.Begin) = 1 then mask := mask + 8; end; "
        "if r.Seek(0, SeekOrigin.End) = 4 then mask := mask + 16; end; "
        "return mask; ")), 31);
}

// §7.1 `ReadAll(): owned Array<byte>` — "reads from current position to end".
BEZEL_TEST(filestream_645_7_1, read_all_reads_from_current_position_to_end) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(1)); src.Add(byte(2)); src.Add(byte(3)); src.Add(byte(4)); "
        "using var w := new FileStream(\"/tmp/bzt645_all.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_all.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "r.Position := 1; "
        "using var rest := r.ReadAll(); "
        "if rest.Count = 3 and rest[0] = byte(2) and rest[2] = byte(4) then "
        "  return 1; end; "
        "return 0; ")), 1);
}

// §7.1 CanRead / CanWrite / CanSeek follow the FileAccess the stream was
// opened with; a regular file is seekable.
BEZEL_TEST(filestream_645_7_1, can_read_can_write_can_seek_follow_file_access) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var w := new FileStream(\"/tmp/bzt645_caps.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "var mask := 0; "
        "if w.CanWrite then mask := mask + 1; end; "
        "if not w.CanRead then mask := mask + 2; end; "
        "if w.CanSeek then mask := mask + 4; end; "
        "w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt645_caps.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "if r.CanRead then mask := mask + 8; end; "
        "if not r.CanWrite then mask := mask + 16; end; "
        "return mask; ")), 31);
}

// §7.2 `public readonly property Path: string;` — the constructor's path.
// runAot (not runAotChecked): a string-valued builtin property mints a +1
// string the compiler does not release, a PRE-EXISTING leak class shared with
// `File.ReadAllText` (see the note on runChecked above).
BEZEL_TEST(filestream_645_7_2, path_property_returns_the_constructor_path) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var w := new FileStream(\"/tmp/bzt645_path.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "var p := w.Path; "
        "if p = \"/tmp/bzt645_path.bin\" then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §7.2 / §28.2 — the disposal contract. These are the leak/UAF-strict tests.
// ---------------------------------------------------------------------------

// §28.2 exactly-once: an explicit `Dispose()` followed by `using var` scope
// exit must dispose the handle EXACTLY once — a second fclose/free would be a
// double-free ASan reports at the allocator.
BEZEL_TEST(filestream_645_7_2, explicit_dispose_then_scope_exit_is_not_a_double_free) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var fs := new FileStream(\"/tmp/bzt645_dd.bin\", "
        "                               FileMode.Create, FileAccess.Write); "
        "fs.Dispose(); "
        "return 5; ")), 5);
}

// §7.2 "the `using var` idiom guarantees this on every exit edge, including
// exceptions" — the handle is disposed when the scope is left by a raise.
BEZEL_TEST(filestream_645_7_2, using_var_disposes_on_the_exception_exit_edge) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  private static method Boom() "
        "    using var fs := new FileStream(\"/tmp/bzt645_exc.bin\", "
        "                                   FileMode.Create, FileAccess.Write); "
        "    throw new InvalidOperationException(\"x\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    try Boom(); return 0; "
        "    catch InvalidOperationException => return 6; end; "
        "  end Main; end C;"), 6);
}

// §7.2: a stream opened and disposed in a loop leaks nothing across iterations
// (the handle, its path copy, and the OS fd are all released each time).
BEZEL_TEST(filestream_645_7_2, repeated_open_and_dispose_leaks_nothing) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  private static method Once() "
        "    using var fs := new FileStream(\"/tmp/bzt645_loop.bin\", "
        "                                   FileMode.Create, FileAccess.Write); "
        "    using var b := new Array<byte>(); b.Add(byte(1)); "
        "    fs.Write(b, 0, 1); "
        "  end Once; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 8 do Once(); i := i + 1; end; "
        "    return 8; "
        "  end Main; end C;"), 8);
}

// §7.5 deferral pin: a failed open (FileMode.Open on a missing file) does NOT
// crash and its disposal is a no-op. This pins the CURRENT behaviour, not a
// spec rule — §7.5's IOException/FileNotFoundException surface is deferred with
// the rest of the §7 error paths (see the header comment).
BEZEL_TEST(filestream_645_7_5, no_crash_on_missing_file_open) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var r := new FileStream(\"/tmp/bzt645_absent_645.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "using var buf := new Array<byte>(4, byte(0)); "
        "var n := r.Read(buf, 0, 4); "
        "if n = 0 and r.Length = 0 then return 7; end; "
        "return 0; ")), 7);
}
