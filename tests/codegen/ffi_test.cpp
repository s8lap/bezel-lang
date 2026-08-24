// Codegen slice CG26 — FFI / extern (§32). An extern block declares
// external C-ABI functions; calls to them lower to direct calls resolved
// against the host process (JIT) or libc (native link). This is the first
// observable stdout. JIT tests use libc functions with return values;
// a native-exe test captures real stdout.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp" // parseAndCheckOwnership — #1718 vacuous-lint verifier
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirCg26() {
    return wtScratchDir("wt-cg26");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile `source` to a temp exe and capture its stdout.
std::string compileAndCapture(const std::string& source) {
    auto dir = wtDirCg26();
    std::string stem = unique_stem("bezel_cg26");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::string out;
    if (r.success) {
        if (FILE* pipe = popen(exe.c_str(), "r")) {
            std::array<char, 256> buf;
            while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
                out.append(buf.data(), n);
            pclose(pipe);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return out;
}

} // namespace

BEZEL_TEST(codegen_cg26, extern_call_returns_value) {
    // libc abs(-5) = 5, resolved against the host process by the JIT.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extern \"c\" public method abs(n: int): int; end; "
        "class C public static method Main(): int "
        "  return abs(0 - 5); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg26, extern_result_used_in_expression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "extern \"c\" public method abs(n: int): int; end; "
        "class C public static method Main(): int "
        "  var a := abs(0 - 20); var b := abs(0 - 22); return a + b; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg26, putchar_writes_stdout) {
    // putchar prints 'H','i','\n' (72, 105, 10) — first real stdout.
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method putchar(c: int): int; end; "
        "class C public static method Main(): int "
        "  _ := putchar(72); var _2 := putchar(105); "
        "  var _3 := putchar(10); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, std::string("Hi\n"));
}

// ---------------------------------------------------------------------------
// §32.5 / stdlib §10 FFI-boundary named types: CString, WString, RawBytes,
// IntPtr, UIntPtr. Issue #340 — before this work these names errored
// "unresolved type name" in extern signatures. These tests assert the
// signatures now RESOLVE (the program compiles and `main` runs), and — for
// CString — that an extern call marshals a Bezel `string` end-to-end.
// ---------------------------------------------------------------------------

// §32.7 / stdlib §10.0: IntPtr / UIntPtr resolve as extern param + return
// types (pointer-sized integers). The block is declared but uncalled; that
// `main` runs at all proves the signatures compiled.
BEZEL_TEST(codegen_ffi_types, intptr_uintptr_resolve_32_7) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method f(a: IntPtr, b: UIntPtr): IntPtr; end; "
        "class C public static method Main(): int return 7; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §32.7 / stdlib §10.0 (#1512): `IntPtr.Zero` — the null-pointer const — as an
// EXPRESSION head, and the `=` null test. This is the shape the spec's own
// §32.6 `getenv` and §32.7 `dlopen` examples require (`if handle = IntPtr.Zero
// then ...`). Before the fix, `IntPtr` errored "unresolved identifier" here.
BEZEL_TEST(codegen_ffi_types, intptr_zero_expr_head_and_equality_32_7) {
    // var p := IntPtr.Zero; if p = IntPtr.Zero then return 0; end; return 1;
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var p := IntPtr.Zero; "
        "  if p = IntPtr.Zero then return 0; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §32.7 / §5.2 / stdlib §10.0 (#1512): the explicit conversion `IntPtr(long)`
// as an expression head, and `<>`. `IntPtr(0L)` equals `IntPtr.Zero`; a
// non-zero conversion does not. Mirrors §5.2's explicit-conversion syntax.
BEZEL_TEST(codegen_ffi_types, intptr_long_conversion_32_7) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var p0 := IntPtr(0L); "
        "  if p0 <> IntPtr.Zero then return 1; end; "
        "  var p5 := IntPtr(5L); "
        "  if p5 = IntPtr.Zero then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §32.7 / stdlib §10.0 (#1512): the UIntPtr counterparts — `UIntPtr.Zero`, the
// `UIntPtr(ulong)` conversion, and `=`/`<>`.
BEZEL_TEST(codegen_ffi_types, uintptr_zero_and_conversion_32_7) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var z := UIntPtr.Zero; "
        "  if z <> UIntPtr.Zero then return 1; end; "
        "  var u := UIntPtr(7UL); "
        "  if u = UIntPtr.Zero then return 2; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §32.7 / §5.2 / stdlib §10.0 (#1512): the reverse conversion `long(IntPtr)`
// round-trips a value through the pointer-sized integer.
BEZEL_TEST(codegen_ffi_types, intptr_long_roundtrip_32_7) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var p := IntPtr(42L); "
        "  return int(long(p)); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §32.7 (#1512): a non-integer argument to the `IntPtr(...)` conversion is a
// compile error (§32.7: conversions are to and from `long`/`ulong`).
BEZEL_TEST(codegen_ffi_types, intptr_conversion_rejects_non_integer_32_7) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var p := IntPtr(\"hello\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
}

// §32.5 / stdlib §10.1: CString resolves as an extern param + return type.
BEZEL_TEST(codegen_ffi_types, cstring_resolve_32_5) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strdup(s: CString): CString; end; "
        "class C public static method Main(): int return 1; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §32.1: a nullable CString return (`CString?`, as in the spec's `strdup`
// example) resolves and is FFI-permitted.
BEZEL_TEST(codegen_ffi_types, nullable_cstring_resolve_32_1) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strdup(s: CString): CString?; end; "
        "class C public static method Main(): int return 2; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §32.5 / stdlib §10.2 / §10.3: WString and RawBytes resolve as extern types.
BEZEL_TEST(codegen_ffi_types, wstring_rawbytes_resolve_32_5) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method w(s: WString): IntPtr; "
        "  public method b(buf: RawBytes): IntPtr; end; "
        "class C public static method Main(): int return 3; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// §32.5 / §32.6: a Bezel `string` argument marshals to a `CString` (char*)
// parameter end-to-end. libc atoi("42") parses the null-terminated UTF-8 data
// pointer of the bz_string and returns 42, observable as the JIT exit code.
BEZEL_TEST(codegen_ffi_types, cstring_arg_marshals_to_libc_atoi_32_6) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method atoi(s: CString): int; end; "
        "class C public static method Main(): int "
        "  return atoi(\"42\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §32.5 / §32.6: a CString argument round-trips through libc puts, which
// writes the string + newline to stdout — observable end-to-end output.
BEZEL_TEST(codegen_ffi_types, cstring_arg_marshals_to_libc_puts_32_6) {
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method puts(s: CString): int; end; "
        "class C public static method Main(): int "
        "  _ := puts(\"hello-ffi\"); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(out, std::string("hello-ffi\n"));
}

// §32.5 / §32.6: a NON-ASCII string (storage width > 1) marshalled to CString
// must be RE-ENCODED to null-terminated UTF-8, not handed over as the raw wide
// data pointer. A width-2 string stores each code point in 2 bytes, so the raw
// buffer has interior NUL bytes (e.g. U+0100 → 0x00 0x01) that truncate a C
// `char*` read. "AĀB" is A (U+0041), Ā (U+0100), B (U+0042); its UTF-8 is
// 0x41 0xC4 0x80 0x42 — 4 bytes. libc strlen over the marshalled CString must
// therefore return 4. Without the re-encode, strlen stops at the first interior
// NUL and returns 1 (silent corruption). Ā forces a single code point
// above 0xFF so the literal lowers to storage width 2.
BEZEL_TEST(codegen_ffi_types, cstring_nonascii_reencoded_to_utf8_strlen_32_6) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var s := \"A\\u0100B\"; "
        "  return strlen(s); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// §32.5 / §32.6: a width-1 (pure-ASCII) string still marshals correctly through
// the same path — strlen("ABC") = 3. Guards against the fast path regressing.
BEZEL_TEST(codegen_ffi_types, cstring_ascii_strlen_unchanged_32_6) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  return strlen(\"ABC\"); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// §32.5 / §32.6: a non-ASCII (width-2) string round-trips through libc puts as
// correct UTF-8 BYTES. "AĀB" → 0x41 0xC4 0x80 0x42 then puts' newline.
// Without the re-encode, puts would emit only the leading 'A' before the
// interior NUL. Observed end-to-end on stdout.
BEZEL_TEST(codegen_ffi_types, cstring_nonascii_puts_utf8_bytes_32_6) {
    std::string out = compileAndCapture(
        "module M; "
        "extern \"c\" public method puts(s: CString): int; end; "
        "class C public static method Main(): int "
        "  _ := puts(\"A\\u0100B\"); return 0; "
        "end Main; end C;");
    // 0x41 0xC4 0x80 0x42 + '\n'
    std::string expected;
    expected.push_back((char)0x41);
    expected.push_back((char)0xC4);
    expected.push_back((char)0x80);
    expected.push_back((char)0x42);
    expected.push_back('\n');
    BEZEL_EXPECT_EQ(out, expected);
}

// ---------------------------------------------------------------------------
// fix #977 / stdlib §10.1 / §32.6: CString.From(s: string) — static factory.
// CString in expression position (bareword as static-call receiver) was
// previously rejected with "unresolved identifier `CString`" even though the
// type resolver accepted `CString` in type-annotation position. These tests
// verify that the sema fix lets the receiver resolve and that codegen lowers
// CString.From via bz_string_to_utf8z, producing a correctly sized null-
// terminated UTF-8 buffer. Each `using var` binding is disposed (freed) at
// scope end.
// ---------------------------------------------------------------------------

// §10.1 / §32.6 / fix #977: CString.From("hello") produces a 5-byte buffer;
// strlen returns 5. Verifies sema resolution and codegen lowering end-to-end.
BEZEL_TEST(codegen_ffi_types, cstring_from_strlen_10_1_977) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  using var cs := CString.From(\"hello\"); "
        "  return strlen(cs); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §10.1 / fix #977: CString.From in a loop — disposal (free) fires 100×
// without corruption. Returns 0 if total length is correct (11 × 100 = 1100;
// capped to byte range: 1100 mod 256 = 76).
BEZEL_TEST(codegen_ffi_types, cstring_from_loop_disposal_10_1_977) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 100 do "
        "    using var cs := CString.From(\"hello world\"); "
        "    total := total + strlen(cs); "
        "    i := i + 1; "
        "  end; "
        "  if total = 1100 then return 76; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 1100 mod 256 = 76 (runJit returns raw i32 < 256 exit code from Main).
    BEZEL_EXPECT_EQ(r.exitCode, 76);
}

// §10.1 / fix #977: CString.From with a non-ASCII string (width-2). The
// produced CString must be re-encoded UTF-8, not raw wide data. "AĀB" is
// 3 code points but 4 UTF-8 bytes (Ā = U+0100 → 0xC4 0x80).
BEZEL_TEST(codegen_ffi_types, cstring_from_nonascii_10_1_977) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  using var cs := CString.From(\"A\\u0100B\"); "
        "  return strlen(cs); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// §2.3: Console is always available; Write/WriteLine are intrinsics lowered
// via POSIX write(2). These tests use compileAndCapture to observe stdout.

BEZEL_TEST(codegen_console, write_string) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.Write(\"Hello\"); return 0; "
        "end Main; end C;"), std::string("Hello"));
}

BEZEL_TEST(codegen_console, writeline_string) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"World\"); return 0; "
        "end Main; end C;"), std::string("World\n"));
}

BEZEL_TEST(codegen_console, writeline_int) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(42); return 0; "
        "end Main; end C;"), std::string("42\n"));
}

BEZEL_TEST(codegen_console, writeline_no_args) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(); return 0; "
        "end Main; end C;"), std::string("\n"));
}

BEZEL_TEST(codegen_console, writeline_interpolated) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  var x := 7; Console.WriteLine($\"x={x}\"); return 0; "
        "end Main; end C;"), std::string("x=7\n"));
}

// Console.Write(bool) routes through emitPrimitiveToString → bool special case.
// Per stdlib bool.ToString() → "True"/"False" (#452: was "true", now "True").
BEZEL_TEST(codegen_console, write_bool) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; class C public static method Main(): int "
        "  Console.Write(true); return 0; "
        "end Main; end C;"), std::string("True"));
}

// §2.3 / stdlib §9: Convert.ToInt32 / Math.Abs / Math.Min / Math.Max
// as always-available built-in class method intrinsics.

BEZEL_TEST(codegen_convert, to_int32_from_string) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"42\"; return Convert.ToInt32(s); "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_math, abs_positive) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int return Math.Abs(5); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_math, abs_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int return Math.Abs(0 - 7); "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_math, min_max) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return Math.Min(3, 7) + Math.Max(3, 7); "
        "end Main; end C;"), 10);
}

// fix #983 / stdlib §10.1: CString.Length is strlen of the underlying buffer
// (byte count excluding the terminator), not a constant 0. Before the fix the
// member read fell through every handler and evaluated to 0.
BEZEL_TEST(codegen_ffi_types, cstring_length_property_983) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var cs := CString.From(\"hello\"); "
        "  return cs.Length; "
        "end Main; end C;"), 5);
}

// fix #983 / stdlib §10.1: Length of a FromBytes-constructed CString — the bytes
// are copied and a terminator appended, so Length is the content length.
BEZEL_TEST(codegen_ffi_types, cstring_frombytes_length_983) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := [104, 105]; "
        "  using var cs := CString.FromBytes(b); "
        "  return cs.Length; "
        "end Main; end C;"), 2);
}

// fix #983 / stdlib §10.2: WString.Length is the UTF-16 code-unit count via
// bz_wstrlen (strlen would stop at the high zero byte of an ASCII unit).
BEZEL_TEST(codegen_ffi_types, wstring_length_property_983) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var w := WString.From(\"hi\"); "
        "  return w.Length; "
        "end Main; end C;"), 2);
}

// fix #985 / stdlib §10.3: RawBytes is a pointer-and-length pair. With the
// { ptr, len } representation, RawBytes.Length reads the stored byte count.
// Before the fix the bare-buffer rep dropped the length and .Length was 0.
BEZEL_TEST(codegen_ffi_types, rawbytes_length_property_985) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := [10, 20, 30, 40]; "
        "  using var rb := RawBytes.From(b); "
        "  return rb.Length; "
        "end Main; end C;"), 4);
}

// fix #985 / stdlib §10.3: RawBytes.From of a 0-length Array<byte> has Length 0
// (the struct is still allocated; the buffer is empty). The malloc(0) edge.
BEZEL_TEST(codegen_ffi_types, rawbytes_empty_length_985) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := []; "
        "  using var rb := RawBytes.From(b); "
        "  return rb.Length; "
        "end Main; end C;"), 0);
}

// fix #985 / stdlib §10.3: RawBytes.CopyTo(destination, destOffset) copies the
// stored bytes into the destination array. Round-trip: From a known array, copy
// back into a fresh array, sum the bytes (5+6+7+8 = 26) to prove every byte
// landed. Before the fix CopyTo was unbacked and copied nothing.
BEZEL_TEST(codegen_ffi_types, rawbytes_copyto_roundtrip_985) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src: Array<byte> := [5, 6, 7, 8]; "
        "  using var rb := RawBytes.From(src); "
        "  var dst: Array<byte> := [0, 0, 0, 0]; "
        "  rb.CopyTo(dst, 0); "
        "  return dst[0] + dst[1] + dst[2] + dst[3]; "
        "end Main; end C;"), 26);
}

// fix #985 / stdlib §10.3: RawBytes.CopyTo honors destOffset — copy [9, 9] into
// a fresh [0,0,0,0] at offset 1 yields [0, 9, 9, 0]; reading dst[1] proves the
// offset placement (and dst[0] stays untouched at 0).
BEZEL_TEST(codegen_ffi_types, rawbytes_copyto_offset_985) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var src: Array<byte> := [9, 9]; "
        "  using var rb := RawBytes.From(src); "
        "  var dst: Array<byte> := [0, 0, 0, 0]; "
        "  rb.CopyTo(dst, 1); "
        "  return dst[0] * 100 + dst[1]; "
        "end Main; end C;"), 9);
}

// fix #985 / stdlib §10.3: RawBytes.Pointer exposes the underlying buffer as an
// IntPtr. For a non-empty RawBytes the buffer is non-null; cast to long and
// compare against 0 to observe it (returns 1 when non-null).
BEZEL_TEST(codegen_ffi_types, rawbytes_pointer_nonnull_985) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := [1, 2, 3]; "
        "  using var rb := RawBytes.From(b); "
        "  var p: IntPtr := rb.Pointer; "
        "  if long(p) <> 0 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// fix #1720 / spec §32.5 (v0.196): a `RawBytes` extern PARAMETER occupies TWO
// consecutive C argument slots — the buffer pointer (const void*) then the
// length (size_t). Before the fix RawBytes lowered to ONE slot (the pointer
// only), dropping the length, so every C argument after the RawBytes was
// shifted by one slot and the callee read a garbage/wrong length.
//
// This pins the slot layout deterministically against a real libc call. write(2)
// is write(int fd, const void* buf, size_t count). We declare a bogus trailing
// `extra` parameter to expose the layout: under the correct two-slot
// marshalling the C call is write(1, ptr, LEN, extra) and write reads the first
// three — (1, ptr, 13) — writing all 13 bytes ("Hello, World!"), with `extra`
// landing in an unused fourth register slot. Under the one-slot bug the call is
// write(1, ptr, extra) and write reads count = extra = 5, writing only "Hello".
// Fails-without → stdout "Hello"; passes-with → stdout "Hello, World!".
BEZEL_TEST(codegen_ffi_types, rawbytes_param_two_slot_marshalling_spec_32_5_1720) {
    BEZEL_EXPECT_EQ(compileAndCapture(
        "module M; "
        "extern \"c\" "
        "  public method write(fd: int, data: RawBytes, extra: long): long; end; "
        "class C public static method Main(): int "
        "  var b: Array<byte> := "
        "    [72, 101, 108, 108, 111, 44, 32, 87, 111, 114, 108, 100, 33]; "
        "  using var rb := RawBytes.From(b); "
        "  _ := write(1, rb, 5); "
        "  return 0; "
        "end Main; end C;"), std::string("Hello, World!"));
}

// fix #1720 / spec §32.5 (v0.196): a `RawBytes` extern RETURN is rejected — a C
// function returns one value and the two-slot (pointer, length) shape has no
// return convention (§32.5 directs declaring the pointer return plus a length
// out-parameter and wrapping with RawBytes.FromForeign). Before the fix
// isFfiCompatibleType admitted `RawBytes` in return position. Fails-without →
// the program compiles and runs; passes-with → checkExternType rejects it with
// the §32.5 diagnostic.
BEZEL_TEST(codegen_ffi_types, rawbytes_extern_return_rejected_spec_32_5_1720) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method bad(): RawBytes; end; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`RawBytes` return type is rejected") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""32.5") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}

// fix #1718 / spec §32.6 / §28.1 (v0.186): a `using var` bound to an extern
// returning `owned CString?` is NOT vacuous — §32.6 says "a `using var`
// discharges it" (the spec's strdup example), and §28.10's vacuous-pin lint
// must not fire on such a binding. Before the fix producerState fell through
// to a Borrowed classification (it never consulted ExternDeclSymbol::returnOwned)
// AND rhsIsDisposableClassType returned false for the CString head, so the
// binding was reported vacuous (a §28.10 lint that mis-stated a code site
// carrying a genuine free() obligation). Fails-without → the §28.10 vacuous
// warning fires; passes-with → the warning does not fire and the program runs.
BEZEL_TEST(codegen_ffi_types, owned_extern_cstring_using_var_not_vacuous_spec_32_6_1718) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var c := strdup(CString.From(\"hello-ffi-owned\")); "
        "  if c = null then return 99; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// fix #1718 / spec §32.6 / §28.1 (v0.186): the ownership pass must classify a
// CallExpr whose callee is an owned-returning EXTERN as `Owned` (so the
// using-var chain reaches the §32.6 "free() discharges it" path end-to-end)
// and the rhsIsDisposableClassType classifier must recognize CString as a
// disposable-buffer type so the §28.10 vacuous-pin lint does not fire on the
// binding. Both lived previously as gaps: producerState looked only at
// MethodDecl::returnOwned / importedReturnOwned (never ExternDeclSymbol::returnOwned),
// and rhsIsDisposableClassType's builtin-head allowlist did not include
// CString/WString/RawBytes. Fails-without → the §28.10 vacuous warning fires;
// passes-with → it does not.
BEZEL_TEST(ownership_extern_1718, owned_extern_cstring_using_var_no_vacuous_lint_spec_32_6) {
    auto r = bezel::test::parseAndCheckOwnership(
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var c := strdup(CString.From(\"hello\")); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // No §28.10 vacuous-pin warning on the using var binding (the fix widened
    // rhsIsDisposableClassType to admit the CString head).
    bool hasVacuous = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("vacuous") != std::string::npos &&
            d.message.find("`c`") != std::string::npos)
            hasVacuous = true;
    }
    BEZEL_EXPECT_FALSE(hasVacuous);
}

// fix #1718 / spec §32.6 / §28.1 (v0.186): a `using var` over WString (also
// a FFI-buffer builtin head) must NOT trigger the §28.10 vacuous lint either.
// The fix's rhsIsDisposableClassType widening covers the whole CString /
// WString / RawBytes allowlist; this case pins the WString branch.
BEZEL_TEST(ownership_extern_1718, owned_extern_wstring_using_var_no_vacuous_lint_spec_32_6) {
    auto r = bezel::test::parseAndCheckOwnership(
        "module M; "
        "extern \"c\" public method some_wcsdup(s: WString): owned WString?; end; "
        "class C public static method Main(): int "
        "  using var w := some_wcsdup(WString.From(\"wide\")); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool hasVacuous = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("vacuous") != std::string::npos &&
            d.message.find("`w`") != std::string::npos)
            hasVacuous = true;
    }
    BEZEL_EXPECT_FALSE(hasVacuous);
}

// fix #1718 / spec §32.6 / §28.1 (v0.186): the strdup'd buffer must be freed
// at scope end when bound by `using var`. The post-fix path registers a
// null-guarded free() at the local's scope end (registerUsingDisposal's
// isFfiBuf branch, which keys on the CString head on the initializer's
// exprType). runAotChecked links with ASan; a leaking strdup'd buffer is
// flagged at exit (LSan). Passes-with → ran=true (no LSan leak); the
// negative path is covered by the existing #977 / #985 rawbytes_free tests
// for the established CString.From / RawBytes.From shapes.
BEZEL_TEST(codegen_ffi_types, owned_extern_cstring_using_var_frees_strdup_buffer_spec_32_6_1718) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strdup(s: CString): owned CString?; end; "
        "class C public static method Main(): int "
        "  using var c := strdup(CString.From(\"hello-1718\")); "
        "  if c = null then return 99; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// fix #2728 / §32.6 + §28.3 *Unbound owned temporaries* + §32.5 (v0.240):
// an FFI buffer minted by `CString.From` / `WString.From` / `RawBytes.From` and
// left UNBOUND — written directly in an extern argument list, or dropped as a
// statement — was never freed by anything. §32.6 puts the obligation on the
// caller ("The `CString` class is for caller-owned strings: those that the
// Bezel side allocated and is responsible for disposing"); §28.3 places the
// reclaim at "the end of the full expression in which it is produced". #1718's
// `using var` covers only a BOUND buffer, and #2222 covers only the buffer the
// compiler mints implicitly for a `string` argument — the explicitly-written
// factory in argument position fell between them.
//
// MEASUREMENT NOTE (why every one of these tests loops 64x). LSan's verdict on
// a SINGLE such leak is binary-layout dependent: the pointer often survives in
// a register or stack slot at exit and is reported as reachable, so the same
// program can pass at one commit and fail at the next with no semantic change
// (this is exactly why #2728 sat undetected — the gate immediately before it
// reported this file green). Looping makes the leak 64-fold: at most the final
// iteration's pointer can be spuriously reachable, so the remaining 63 are
// reported unconditionally. Each test also pins a VALUE through the exit code,
// so a premature free (which corrupts what the C callee reads) and a double
// free (which aborts, clearing `ran`) are caught independently of the sanitizer.
//
// Measured on the pre-fix compiler, all five: A 16448 B/64 allocs, B 12160 B/64,
// C 1536 B/128 (RawBytes is two allocations — the {ptr,len} struct and its
// buffer — hence the bz_rawbytes_free reclaim), D 13120 B/64, E 12864 B/64.
// ---------------------------------------------------------------------------

// §32.6 / §28.3 (v0.240): CString.From written directly in an extern ARGUMENT
// list. The temporary is bound to no name, so §28.3's unbound-temporary rule
// governs and it must be freed once the call is finished with it. Value pin:
// 64 iterations x strlen(64-char literal) = 4096.
BEZEL_TEST(codegen_ffi_types, cstring_from_argument_temporary_freed_spec_32_6_2728) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 64 do "
        "    total := total + strlen(CString.From(\"abcdefghijklmnopqrstuvwxyz"
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ01\")); "
        "    i := i + 1; "
        "  end; "
        "  if total = 4096 then return 21; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 21);
}

// §32.6 / §28.3 (v0.240): the WString family, same position. The declared C
// signature is deliberately `strlen` over a WString — reading the UTF-16LE
// buffer as bytes stops at the first NUL byte, which for an ASCII-leading
// literal is index 1 and well inside the buffer, so the read is in bounds and
// deterministic (§32.10 puts signature fidelity on the programmer; here the
// point under test is the BUFFER's reclaim, not the callee). Pin: 64 x 1 = 64.
BEZEL_TEST(codegen_ffi_types, wstring_from_argument_temporary_freed_spec_32_6_2728) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: WString): int; end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 64 do "
        "    total := total + strlen(WString.From(\"wide-argument-temporary-"
        "abcdefghijklmnopqrstuvw\")); "
        "    i := i + 1; "
        "  end; "
        "  if total = 64 then return 22; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

// §32.5 / §28.3 (v0.240): the RawBytes family, same position. §32.5: a RawBytes
// parameter "occupies two consecutive C argument slots: the pointer
// (`const void*`), then the length (`size_t`)" — so `write(fd, data)` lowers to
// write(int, const void*, size_t). fd = -1 makes the call fail with EBADF
// without writing anything, keeping the test's stdout clean while still passing
// the marshalled buffer across the boundary. This case needs the TWO-level
// bz_rawbytes_free (struct + buffer): pre-fix it leaked 128 allocations for 64
// iterations, which a plain free() reclaim would have only halved.
BEZEL_TEST(codegen_ffi_types, rawbytes_from_argument_temporary_freed_spec_32_5_2728) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method write(fd: int, data: RawBytes): long; end; "
        "class C public static method Main(): int "
        "  var src: Array<byte> := [1, 2, 3, 4, 5, 6, 7, 8]; "
        "  var i := 0; "
        "  while i < 64 do "
        "    _ := write(-1, RawBytes.From(src)); "
        "    i := i + 1; "
        "  end; "
        "  return 23; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 23);
}

// §28.3 + §21.9 (v0.240): the DISCARDED-EXPRESSION-STATEMENT position — a bare
// `CString.From(s);` on a line of its own. §28.3 fixes the boundary: "in an
// expression statement, 'end of the full expression' and 'end of the statement'
// coincide (§21.9)". This is the FFI counterpart of #2655's bare `new T();`,
// and it reaches a different codegen arm: the ownership pass classifies the FFI
// factories Borrowed, so #2655's flag is never set for them.
BEZEL_TEST(codegen_ffi_types, cstring_from_bare_statement_temporary_freed_spec_28_3_2728) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 64 do "
        "    CString.From(\"bare-statement-temporary-abcdefghijklmnopqrstuvwxyz\"); "
        "    i := i + 1; "
        "  end; "
        "  return 24; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 24);
}

// §17.13 + §28.3 (v0.240): the same statement written with the discard sink.
// §17.13's discard is a discharge point; neither the #1009 `disposeOwnedValue`
// arm (Borrowed classification) nor the #1015 collection-materializer helper
// covered an FFI buffer, so this spelling leaked too.
BEZEL_TEST(codegen_ffi_types, cstring_from_discard_statement_temporary_freed_spec_17_13_2728) {
    auto r = runAotChecked(
        "module M; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 64 do "
        "    _ := CString.From(\"discard-statement-temporary-abcdefghijklmnopqrstuv\"); "
        "    i := i + 1; "
        "  end; "
        "  return 25; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// fix #2728 — the anti-PREMATURE-FREE half, on the PLAIN (non-sanitized) path.
// The reclaim must land strictly AFTER the callee has read the buffer (§28.3:
// "after every use of it within that expression has completed"). This is #2728's
// own repro plus a length read: `strdup` copies the argument temporary, so if
// the temporary were freed before the call — or freed twice — the copy would be
// garbage or the allocator would abort. Pinning strlen of the COPY at 10
// ("hello-2728") proves the buffer was intact at the boundary, and `ran` proves
// no abort. Deliberately `runAot`, not `runAotChecked`: the value assertion has
// to hold without a sanitizer underneath it.
//
// ⚠ THE NAME OVERCLAIMS, per the #2728 review — read this before trusting it as
// a premature-free oracle. `runAot` has no sanitizer and glibc does not scrub
// freed memory, so hoisting the free before `strdup` would still very likely
// read back an intact "hello-2728" and yield strlen == 10. What this case
// genuinely detects is a DOUBLE FREE (glibc aborts, clearing `ran`). The actual
// premature-free coverage comes from the ASan-linked runAotChecked cases above.
// Kept under this name so the #2728 filter stays stable; do not add a second
// case relying on plain runAot to catch an early free.
BEZEL_TEST(codegen_ffi_types, cstring_from_argument_temporary_not_freed_early_spec_28_3_2728) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method strdup(s: CString): owned CString?; "
        "  public method strlen(s: CString): int; "
        "end; "
        "class C public static method Main(): int "
        "  using var c := strdup(CString.From(\"hello-2728\")); "
        "  if c = null then return 99; end; "
        "  return strlen(c!); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// fix #2728 negative control — a BOUND buffer must still be freed exactly once,
// by the `using var` path alone. §28.3: "A temporary that is instead bound (to a
// `var`/`using var`), `move`d, returned, or transferred into an owning location
// is governed by the corresponding transfer or discharge rule and is **not**
// disposed under this rule (it would otherwise be disposed twice)." Passing the
// BINDING to the same extern must therefore add no second free — which is why
// the #2728 predicate keys on a directly-written factory call and not on the
// argument's CString TYPE. A double free aborts and clears `ran`.
// Pin: 64 x strlen("hello world") = 704.
BEZEL_TEST(codegen_ffi_types, cstring_bound_argument_not_double_freed_spec_28_3_2728) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 64 do "
        "    using var cs := CString.From(\"hello world\"); "
        "    total := total + strlen(cs); "
        "    i := i + 1; "
        "  end; "
        "  if total = 704 then return 26; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 26);
}

// ---------------------------------------------------------------------------
// fix #2733 — the FFI-argument buffer reclaim at §28.3's boundary (v0.240).
//
// Two faces of one defect, both measured on the pre-fix compiler (main 1012daf):
//
//   FACE 1 — PREMATURE FREE. Both reclaims (#2222's implicit `string`->CString
//   marshal buffer and #2728's explicit `CString.From(...)` temporary) were
//   emitted immediately after the callee's CreateCall. §28.3 v0.240 does not
//   license that: "Such a temporary is disposed at the **end of the full
//   expression** in which it is produced, after every use of it within that
//   expression has completed", and "Disposal runs *after* the borrowing use, so
//   the receiver is live for the duration of the call." The boundary is a FIXED
//   POINT, not an upper bound — "after every use has completed" is a conjunct of
//   "at the end of the full expression", not an alternative to it. An inner
//   extern call is lowered inside an OUTER call's argument list, so the inner
//   post-call free landed BEFORE the outer CreateCall: `strlen(strchr(...))`
//   read freed memory. Pre-fix both spellings aborted with
//   "AddressSanitizer: heap-use-after-free ... in strlen".
//
//   FACE 2 — NO UNWIND RECLAIM. The post-call free was on the normal path only,
//   so an argument evaluated LATER that threw leaked the already-produced
//   buffer, against §28.3's "every already-produced, not-yet-consumed unbound
//   owned temporary is disposed during unwinding by the same implicit
//   try/finally mechanism as named locals ... each temporary's disposal is
//   established at its production, so the pass runs in reverse order of
//   production on the establishment-ordered cleanup stack". Pre-fix: 5607 B in
//   63 allocations (CString.From) and 5696 B in 64 (implicit marshal).
//
// THE MEASUREMENT DISCIPLINE (inherited from #2728, and load-bearing here).
// Every leak case loops 64x, because LSan's verdict on a SINGLE such leak is
// binary-layout dependent — the pointer often survives in a register at exit and
// is reported reachable. At 64x at most the last iteration can be spuriously
// reachable, so a green run positively establishes that >=63 buffers were
// genuinely reclaimed; it is not merely the absence of a report. Each case also
// pins a VALUE through the exit code, so a premature free (garbage read by the
// callee) and a double free (glibc/ASan abort, clearing `ran`) are caught
// independently of the leak checker.
// ---------------------------------------------------------------------------

// FACE 1, the #2728 spelling. `strchr` returns an interior pointer INTO its own
// argument, so the outer `strlen` is a use of the temporary that completes after
// the inner call returns — §28.3 keeps the buffer live for it. 'e' is 101;
// "hello-2733-alpha" from index 1 is "ello-2733-alpha", 15 bytes. 64 x 15 = 960.
// Pre-fix: heap-use-after-free in strlen (r.ran false).
BEZEL_TEST(codegen_ffi_types,
           cstring_from_arg_temp_outlives_aliasing_extern_return_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" "
        "  public method strchr(s: CString, c: int): CString; "
        "  public method strlen(s: CString): int; "
        "end; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 64 do "
        "    total := total + strlen(strchr(CString.From(\"hello-2733-alpha\"), 101)); "
        "    i := i + 1; "
        "  end; "
        "  if total = 960 then return 31; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 31);
}

// FACE 1 on the PLAIN, UNSANITIZED path — deliberately `runAot`, not
// `runAotChecked`. #2728's premature-free case had to disclaim itself ("glibc
// does not scrub freed memory, so hoisting the free would still very likely read
// back an intact string"); an ALIASING return removes that weakness and makes
// the plain oracle real. `strchr` returns a pointer at OFFSET 1 into the buffer,
// and glibc's free() writes tcache metadata (next pointer, then key) over the
// first 16 bytes of the reclaimed chunk — precisely the bytes the outer `strlen`
// then reads. Measured on the pre-fix compiler with no sanitizer linked at all:
// exit 1 (the wrong-length fallthrough); post-fix exit 41. So this case pins the
// §28.3 boundary without ASan underneath it, and a double free would abort in
// glibc and clear `ran`.
BEZEL_TEST(codegen_ffi_types,
           cstring_from_arg_temp_aliasing_return_intact_plain_spec_28_3_2733) {
    auto r = runAot(
        "module M; "
        "extern \"c\" "
        "  public method strchr(s: CString, c: int): CString; "
        "  public method strlen(s: CString): int; "
        "end; "
        "class C public static method Main(): int "
        "  var n := strlen(strchr(CString.From(\"hello-2733-alpha\"), 101)); "
        "  if n = 15 then return 41; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// FACE 1, the #2222 spelling — the buffer the compiler mints IMPLICITLY when a
// Bezel `string` is passed to a `CString` parameter. Reaching it requires a
// code point above 255: a width-1 string hands over its raw data pointer with no
// allocation at all (the ASCII fast path), so only a width>1 string takes the
// bz_string_to_utf8z branch that allocates. 'g' is 103; "gamma" is 5 bytes.
// 64 x 5 = 320. Pre-fix: heap-use-after-free in strlen (r.ran false).
BEZEL_TEST(codegen_ffi_types,
           string_marshal_arg_temp_outlives_aliasing_extern_return_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" "
        "  public method strchr(s: CString, c: int): CString; "
        "  public method strlen(s: CString): int; "
        "end; "
        "class C public static method Main(): int "
        "  var s: string := \"hi\\u65e5-2733-gamma\"; "
        "  var total := 0; var i := 0; "
        "  while i < 64 do "
        "    total := total + strlen(strchr(s, 103)); "
        "    i := i + 1; "
        "  end; "
        "  if total = 320 then return 32; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);
}

// FACE 2, the #2728 spelling: the buffer is produced, then a LATER argument
// throws before the extern call runs. §28.3's exception clause names exactly
// this shape ("in `Combine(new Connection(\"a\"), Fail())` where `Fail()`
// throws, the already-produced `Connection` ... is disposed during the unwind").
// The catch is INSIDE the loop, so each iteration also leaves the block normally
// afterwards — which is the double-free control for the reclaim's
// null-after-free (an unwind-edge free followed by a normal block-end free of
// the same slot would abort under ASan and clear `ran`).
// Pre-fix: 5607 B leaked in 63 allocations.
BEZEL_TEST(codegen_ffi_types,
           cstring_from_arg_temp_reclaimed_when_later_arg_throws_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strchr(s: CString, c: int): IntPtr; end; "
        "class C "
        "  public static method Boom(): int "
        "    throw new Exception(\"boom-2733\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var caught := 0; var i := 0; "
        "    while i < 64 do "
        "      try "
        "        var p := strchr(CString.From(\"leak-2733-beta-payload\"), Boom()); "
        "      catch "
        "        var e: Exception => caught := caught + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    if caught = 64 then return 33; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// FACE 2, the #2222 spelling — same throwing-later-argument shape over the
// implicitly-minted marshal buffer (width>1 string, as above).
// Pre-fix: 5696 B leaked in 64 allocations.
BEZEL_TEST(codegen_ffi_types,
           string_marshal_arg_temp_reclaimed_when_later_arg_throws_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strchr(s: CString, c: int): IntPtr; end; "
        "class C "
        "  public static method Boom(): int "
        "    throw new Exception(\"boom-2733-delta\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var s: string := \"hi\\u65e5-2733-delta-payload\"; "
        "    var caught := 0; var i := 0; "
        "    while i < 64 do "
        "      try "
        "        var p := strchr(s, Boom()); "
        "      catch "
        "        var e: Exception => caught := caught + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    if caught = 64 then return 34; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 34);
}

// FACE 2 over the RawBytes family, which is also the strongest DOUBLE-FREE
// control in this group: a RawBytes buffer is a TWO-level allocation reclaimed
// by bz_rawbytes_free (the {ptr,len} struct AND its byte buffer, stdlib §10.3 /
// #985), so reclaiming it twice — once on the unwind edge and again at the
// enclosing block's normal end after the catch — aborts rather than merely
// leaking. §32.5: a RawBytes parameter "occupies two consecutive C argument
// slots: the pointer (`const void*`), then the length (`size_t`)", so this
// declaration lowers to the real pwrite(int, const void*, size_t, off_t); the
// buffer must be produced by an EARLIER argument than the thrower, which is why
// a 3-parameter extern is needed here (write/2 puts the buffer last).
// fd = -1 keeps the call harmless; it never runs anyway, since Boom() throws
// while evaluating the last argument.
BEZEL_TEST(codegen_ffi_types,
           rawbytes_from_arg_temp_reclaimed_when_later_arg_throws_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" "
        "  public method pwrite(fd: int, data: RawBytes, off: long): long; "
        "end; "
        "class C "
        "  public static method Boom(): long "
        "    throw new Exception(\"boom-2733-eps\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var src: Array<byte> := [1, 2, 3, 4, 5, 6, 7, 8]; "
        "    var caught := 0; var i := 0; "
        "    while i < 64 do "
        "      try "
        "        _ := pwrite(-1, RawBytes.From(src), Boom()); "
        "      catch "
        "        var e: Exception => caught := caught + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    if caught = 64 then return 35; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 35);
}

// §28.3 (v0.240) — the NESTED-FUNCTION-BODY boundary. A lambda body is lowered
// while the ENCLOSING expression is still open, so it never reaches the
// outermost-expression boundary of the function it is actually emitted into.
// This case is here because #2733's own first draft leaked exactly this shape
// (measured: 64 x 85 B) after it moved the reclaim off the post-call site — the
// pre-#2733 post-call free happened to cover it by accident of placement. It is
// a REGRESSION GUARD, not a pre-existing-bug repro: it is green on main and must
// stay green. Pin: 64 x strlen("lambda-axis-2733-only") = 64 x 21 = 1344.
BEZEL_TEST(codegen_ffi_types,
           cstring_from_arg_temp_in_lambda_body_reclaimed_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var f := lambda(): int => strlen(CString.From(\"lambda-axis-2733-only\")); "
        "  var t := 0; var i := 0; "
        "  while i < 64 do "
        "    t := t + f(); "
        "    i := i + 1; "
        "  end; "
        "  if t = 1344 then return 37; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 37);
}

// §28.3 K8.2 (v0.240) — the NO-REGRESSION case for WHERE the boundary is put.
// "Each **evaluation** of a condition, `when` arm condition, or pattern guard is
// its own boundary (K8.2), so condition temporaries are disposed per evaluation
// and never accumulate across iterations." A boundary hooked at STATEMENT
// granularity would flush a `while` condition's temporary only once, after the
// loop — leaking every iteration but the last, a REGRESSION against the
// post-call free this fix replaces. This pins per-evaluation reclaim:
// "loop-condition-temporary-2733-payload" is 37 bytes, so the condition
// `i + 37 < 100` runs the body for i = 0..62 and is evaluated 64 times.
BEZEL_TEST(codegen_ffi_types,
           cstring_from_loop_condition_temp_reclaimed_per_evaluation_spec_28_3_2733) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i + strlen(CString.From("
        "      \"loop-condition-temporary-2733-payload\")) < 100 do "
        "    i := i + 1; "
        "  end; "
        "  if i = 63 then return 36; end; "
        "  return 1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 36);
}
