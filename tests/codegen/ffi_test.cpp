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
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Compile `source` to a temp exe and capture its stdout.
std::string compileAndCapture(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_cg26_" + std::to_string(counter++);
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
        "  var _ := putchar(72); var _2 := putchar(105); "
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
    auto r = runJit(
        "module M; "
        "extern \"c\" "
        "  public method f(a: IntPtr, b: UIntPtr): IntPtr; end; "
        "class C public static method Main(): int return 7; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §32.5 / stdlib §10.1: CString resolves as an extern param + return type.
BEZEL_TEST(codegen_ffi_types, cstring_resolve_32_5) {
    auto r = runJit(
        "module M; "
        "extern \"c\" public method strdup(s: CString): CString; end; "
        "class C public static method Main(): int return 1; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §32.1: a nullable CString return (`CString?`, as in the spec's `strdup`
// example) resolves and is FFI-permitted.
BEZEL_TEST(codegen_ffi_types, nullable_cstring_resolve_32_1) {
    auto r = runJit(
        "module M; "
        "extern \"c\" public method strdup(s: CString): CString?; end; "
        "class C public static method Main(): int return 2; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §32.5 / stdlib §10.2 / §10.3: WString and RawBytes resolve as extern types.
BEZEL_TEST(codegen_ffi_types, wstring_rawbytes_resolve_32_5) {
    auto r = runJit(
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
    auto r = runJit(
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
        "  var _ := puts(\"hello-ffi\"); return 0; "
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
    auto r = runJit(
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
    auto r = runJit(
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
        "  var _ := puts(\"A\\u0100B\"); return 0; "
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
    auto r = runJit(
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
    auto r = runJit(
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
    auto r = runJit(
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
