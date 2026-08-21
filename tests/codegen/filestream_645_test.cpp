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
// Paths are unique per test (/tmp/bzt645_*) to avoid cross-test interference —
// the same convention as file_io_644_test.cpp. No two tests here share a path,
// and no test depends on a file another test left behind (verified by running
// the whole filter from a cleaned /tmp).
//
// If one of the runAotChecked cases below ever fails with a bare -999 (`ran` is
// false), suspect BUILD CONTENTION before suspecting the code: these are the
// most expensive links in the file (ASan-instrumented), so they are the first
// to hit the harness's compile deadline when something else is rebuilding the
// same tree. Re-run the suspect in isolation — `BEZEL_TEST_FILTER=<name>`, a
// single-process run, with no concurrent build — before believing it.

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
// The path is under a directory that does not exist, so the open failure is
// STRUCTURAL: unlike a plain unused filename, no stray probe or future test can
// accidentally satisfy the precondition and turn this green for the wrong
// reason.
BEZEL_TEST(filestream_645_7_5, no_crash_on_missing_file_open) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var r := new FileStream(\"/tmp/bzt645_no_such_dir/absent.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "using var buf := new Array<byte>(4, byte(0)); "
        "var n := r.Read(buf, 0, 4); "
        "if n = 0 and r.Length = 0 then return 7; end; "
        "return 0; ")), 7);
}

// ---------------------------------------------------------------------------
// fix #2617 — §7.4 ("Member access on nullable values", v0.236) narrowed
// nullable operand to the stdlib §7.1 `Position: long` setter.
//
// §7.4 "How narrowing is established" admits an early exit: "after
// `if x = null then return; end;` … the surviving path is narrowed", and the
// null test form `x <> null` narrows the true branch. Sema therefore accepts a
// narrowed `long?` (and, through §6's `S → T → T?` numeric widening, a narrowed
// `int?`) as the value of the non-nullable `long` Position property.
//
// Codegen did NOT unwrap it: the narrowed local keeps its declared tagged-
// optional `{i8 hasVal; T}` layout, which was handed straight to
// bz_filestream_set_position's `i64` parameter, so LLVM's verifyModule rejected
// the module — "Call parameter type does not match function signature!" — i.e.
// a codegen ICE on a legal §7.4 idiom. Same family as #2605 (atomics) / #2614.
//
// WITHOUT the fix every case below fails to COMPILE (`ran` is false, run()
// yields -999); WITH it each returns its distinct non-zero code. The codes are
// runtime VALUE assertions derived from the bytes actually read back at the
// seeked-to position, so a value-corrupting unwrap (e.g. extracting the hasVal
// flag instead of the payload, or truncating) cannot be mistaken for a pass.
// Verified mechanically both ways (patch out → -999; patch in → the code).
// ---------------------------------------------------------------------------

// §7.4 early-exit narrowing: `if p = null then return; end;` narrows `p` on the
// surviving path, and the narrowed `long?` is admitted for `Position: long`.
// The assertion is on the SEEK's effect (the byte read back), not just on the
// Position getter, so an unwrap that produced the wrong integer would fail.
BEZEL_TEST(filestream_2617_7_4, narrowed_long_nullable_position_set) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(10)); src.Add(byte(11)); "
        "src.Add(byte(12)); src.Add(byte(13)); "
        "using var w := new FileStream(\"/tmp/bzt2617_long.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_long.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var p: long? := 2L; "
        "if p = null then return 0; end; "
        "r.Position := p; "
        "if r.Position <> 2 then return 1; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(12) then return 21; end; "
        "return 2; ")), 21);
}

// §7.4 + §6 numeric widening: a narrowed `int?` is likewise admitted (an `int`
// converts to `long`, and the conversion lifts through `?`). Its payload is
// i32, NOT i64 — so this case pins that the unwrap keys on the RHS's own
// declared payload type rather than on the setter's i64 parameter. It ICEd
// with `{i8, i32}` on origin/main, a shape the issue as filed did not name.
BEZEL_TEST(filestream_2617_7_4, narrowed_int_nullable_position_set) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(10)); src.Add(byte(11)); "
        "src.Add(byte(12)); src.Add(byte(13)); "
        "using var w := new FileStream(\"/tmp/bzt2617_int.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_int.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var q: int? := 3; "
        "if q = null then return 0; end; "
        "r.Position := q; "
        "if r.Position <> 3 then return 1; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(13) then return 22; end; "
        "return 2; ")), 22);
}

// §7.4 the other narrowing form — a `<> null` test narrowing its TRUE branch,
// with the set inside the controlled block (the issue's repro used the early
// exit; this pins the block form reaches the same lowering).
BEZEL_TEST(filestream_2617_7_4, narrowed_long_nullable_position_set_in_if_block) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(10)); src.Add(byte(11)); "
        "src.Add(byte(12)); src.Add(byte(13)); "
        "using var w := new FileStream(\"/tmp/bzt2617_ifblk.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_ifblk.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var p: long? := 1L; "
        "if p <> null then r.Position := p; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(11) then return 23; end; "
        "return 2; ")), 23);
}

// §7.11 "Lifted operators": an arithmetic expression over a NARROWED operand
// (`p + 1L`) reaches the setter as an `insertvalue {i8, i64}` rather than as a
// plain optional load — the same defect through a different lowering. Found by
// this lane's adversarial grid, not named in the issue. Sema rejects the
// UN-narrowed form of the same expression ("the assigned value is nullable but
// the target type is non-nullable (§7)", confirmed mechanically), so the
// unwrap here is on a value sema proved non-null.
// §7.11 / §7.4 (v0.286, #3298): `p` is narrowed twice over — by its non-null
// literal initialiser and by the `if p = null then return 0; end;` guard — so
// `p + 1L` is the ordinary `long` addition and the setter receives a `long`.
// #2617's subject (the narrowed-nullable value reaching a `long` property
// setter) is unchanged, and this row keeps its original value assertion; a
// draft of #3298 flipped it to "expect rejection" and deleted that coverage.
BEZEL_TEST(filestream_2617_7_4, narrowed_nullable_in_lifted_operator_position_set) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(10)); src.Add(byte(11)); "
        "src.Add(byte(12)); src.Add(byte(13)); "
        "using var w := new FileStream(\"/tmp/bzt2617_lift.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_lift.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var p: long? := 1L; "
        "if p = null then return 0; end; "
        "r.Position := p + 1L; "
        "if r.Position <> 2 then return 1; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(12) then return 24; end; "
        "return 2; ")), 24);
}

// ---------------------------------------------------------------------------
// fix #2617, part 2 — §5.1 (v0.236) UNSIGNED widening at the same setter.
//
// §5.1's implicit-conversion table admits `byte`, `ushort`, `uint`, and `char`
// -> `long` ("Widening from unsigned to a strictly larger signed type … is
// permitted because no value is lost"), and each lowers to an integer narrower
// than the i64 bz_filestream_set_position takes. The widen at that site was an
// unconditional CreateSExt, which SIGN-extends them: `byte(200)` (i8 0xC8)
// became -56.
//
// For a PLAIN unsigned RHS this is pre-existing. For a NARROWED unsigned
// nullable it was newly REACHABLE: the unwrap added by the first half of this
// fix is what lets such a value reach the widen at all — on origin/main that
// shape died loudly at verifyModule. Without the signedness fix, shipping the
// unwrap would have converted a loud ICE into a silent wrong seek offset. The
// two halves therefore ship together.
//
// These cases assert on the byte read back at the seeked-to offset (or, where
// the offset is far past EOF, on the Position getter), so a sign-extended
// offset cannot pass: -56 is not 200.
// ---------------------------------------------------------------------------

// §5.1 + §7.4: a narrowed `byte?` of 200 has the i8 sign bit set. With SExt the
// seek target is -56; with ZExt it is 200. A 256-byte file whose byte at offset
// N has value N makes the read-back decisive.
BEZEL_TEST(filestream_2617_7_4, narrowed_byte_nullable_position_set_zero_extends) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "var i := 0; "
        "while i < 256 do src.Add(byte(i)); i := i + 1; end; "
        "using var w := new FileStream(\"/tmp/bzt2617_byte.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 256); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_byte.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var b: byte? := byte(200); "
        "if b = null then return 0; end; "
        "r.Position := b; "
        "if r.Position <> 200 then return 1; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(200) then return 25; end; "
        "return 2; ")), 25);
}

// §5.1 + §7.4: a narrowed `ushort?` of 40000 has the i16 sign bit set
// (SExt -> -25536, ZExt -> 40000). The offset is past EOF, where a seek is
// legal on a regular file but no byte can be read back, so the assertion is on
// the Position getter — still decisive, since a negative offset cannot report
// 40000.
BEZEL_TEST(filestream_2617_7_4, narrowed_ushort_nullable_position_set_zero_extends) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(1)); src.Add(byte(2)); "
        "using var w := new FileStream(\"/tmp/bzt2617_ushort.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 2); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_ushort.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var s: ushort? := ushort(40000); "
        "if s = null then return 0; end; "
        "r.Position := s; "
        "if r.Position = 40000 then return 26; end; "
        "return 1; ")), 26);
}

// §5.1 + §7.4: a narrowed `uint?` exactly at the 32-bit sign boundary
// (0x80000000 = 2147483648). SExt -> -2147483648, ZExt -> 2147483648. This is
// the widest unsigned type §5.1 admits here, and the boundary value is the one
// where the two widenings differ maximally.
BEZEL_TEST(filestream_2617_7_4, narrowed_uint_nullable_position_set_zero_extends) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(1)); src.Add(byte(2)); "
        "using var w := new FileStream(\"/tmp/bzt2617_uint.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 2); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_uint.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var u: uint? := uint(2147483648L); "
        "if u = null then return 0; end; "
        "r.Position := u; "
        "if r.Position = 2147483648L then return 27; end; "
        "return 1; ")), 27);
}

// §5.1: the PLAIN (never-nullable) `byte` RHS — the PRE-EXISTING half of the
// sign-extension defect. This fix does not create it, but the fix sits on the
// same widen, so it is pinned here rather than left to regress silently.
BEZEL_TEST(filestream_2617_7_4, plain_byte_position_set_zero_extends) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "var i := 0; "
        "while i < 256 do src.Add(byte(i)); i := i + 1; end; "
        "using var w := new FileStream(\"/tmp/bzt2617_pbyte.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 256); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_pbyte.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var c: byte := byte(200); "
        "r.Position := c; "
        "if r.Position <> 200 then return 1; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(200) then return 28; end; "
        "return 2; ")), 28);
}

// Guard for the OTHER direction: a PLAIN (never-nullable) `long`/`int` RHS must
// still lower exactly as before — the unwrap is a no-op for it. This one
// passes both without and with the fix by construction; it is here so that a
// future change to the unwrap that over-fires (stripping a non-optional value)
// is caught in the same file. Both widths are exercised, since the i32 RHS is
// the one the SExt path handles.
BEZEL_TEST(filestream_2617_7_4, plain_long_and_int_position_set_unchanged) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "using var src := new Array<byte>(); "
        "src.Add(byte(10)); src.Add(byte(11)); "
        "src.Add(byte(12)); src.Add(byte(13)); "
        "using var w := new FileStream(\"/tmp/bzt2617_plain.bin\", "
        "                              FileMode.Create, FileAccess.Write); "
        "w.Write(src, 0, 4); w.Dispose(); "
        "using var r := new FileStream(\"/tmp/bzt2617_plain.bin\", "
        "                              FileMode.Open, FileAccess.Read); "
        "var mask := 0; "
        "var a: long := 2L; "
        "r.Position := a; "
        "if r.Position = 2 then mask := mask + 1; end; "
        "var b: int := 1; "
        "r.Position := b; "
        "if r.Position = 1 then mask := mask + 2; end; "
        "using var buf := new Array<byte>(1, byte(0)); "
        "var n := r.Read(buf, 0, 1); "
        "if n = 1 and buf[0] = byte(11) then mask := mask + 4; end; "
        "return mask; ")), 7);
}
