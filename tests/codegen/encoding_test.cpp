// Codegen + runtime — Encoding (stdlib §3.2). Implements the UTF-8 concrete
// encoding reached through the `Encoding.UTF8` singleton:
//
//   public abstract class Encoding
//     public abstract method GetBytes(s: string): owned Array<byte>;       // §3.2
//     public abstract method GetString(bytes: Array<byte>): string;        // §3.2
//     public static readonly property UTF8: Encoding;                       // §3.2
//   end Encoding;
//
// fix #3851 (stdlib §3.2 / §1, v0.290, proposal 0123 R5 Option B): the
// windowed `GetString(bytes, index, count)` call form shown above in earlier
// revisions of this comment was retired — a window over an Array<byte> is a
// slice, not a parameter pair (§4.14's no-offset-parameter rule); the
// replacement spelling is `GetString(bytes.AsReadOnlySpan(start, length))`.
//
// Scope of THIS milestone: `Encoding.UTF8` only. The UTF16/UTF32/ASCII
// singletons are rejected in sema (their GetBytes/GetString semantics —
// endianness, surrogate handling, ASCII out-of-range — are not pinned down by
// §3.2), so a sound implementation is deferred rather than guessed (CLAUDE.md §4).
//
// Three layers:
//   - Runtime: bz_encoding_utf8_get_bytes / bz_encoding_utf8_get_string
//     (bezel_runtime.cpp). GetBytes produces an owned Array<byte> (bz_array_t*,
//     elem_size 1); GetString produces a fresh +1 managed string (rc := 1).
//   - Codegen: tryLowerEncodingCall (LlvmBackend_Calls.cpp) — marshals the
//     `Encoding.UTF8.GetBytes/GetString` two-level member-access shape.
//   - Sema: types `Encoding.UTF8` (the singleton) and the GetBytes/GetString
//     results (ExprTypecheck_Calls.cpp).
//
// Value tests run through the JIT harness (exit code = Main()'s return). The
// GetString path ALSO gets a no-corruption (ASan) check: a string produced by a
// call is conservatively left leaked by the codegen (the documented string
// lifetime model — see string_refcount_test.cpp), so we assert no UAF / double-
// free (expectValueAndNoCorruption) rather than leak-freedom. The GetBytes path,
// whose owned Array<byte> IS released by `using var`, gets a full leak-free check.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"   // fix #3892: diagnosticMessageContains / failedAtCompileTimeNotRuntime
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True if the program failed to compile (a diagnostic was reported and no
// module was produced). Used by the rejection tests for UTF16/UTF32/ASCII.
bool compileFails(const std::string& source) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return true;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    return diags.hasErrors();
}

// Lower `source` to an object file via the sema+codegen pipeline; returns true
// on success (object written to objPath).

// Build + link `source` against an ASan-built runtime and run it under ASan.
// `detectLeaks` selects leak detection. Returns:
//   the program's exit code on a clean run,
//   99                       on an ASan-reported error (UAF / double-free / leak),
//   -1                       if the toolchain is unavailable (SKIP).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderAsan(const std::string& source, bool detectLeaks) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsanExitCode(source, "bezel_enc", detectLeaks);
}

// Assert the value AND that the GetBytes byte-array path is fully leak-free
// (the owned Array<byte> is discharged by `using var`; no string is produced).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int code = runUnderAsan(source, /*detectLeaks=*/true);
    if (code == -1) return;                 // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(code, expectedExitCode);  // no leak, no corruption
}

// Assert the value AND that the GetString allocating path has no memory-safety
// error (UAF / double-free). A leak is tolerated: a string produced by a call is
// conservatively left leaked by codegen (the documented string lifetime model),
// so we run with leak detection OFF and require a clean exit at the right value.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int code = runUnderAsan(source, /*detectLeaks=*/false);
    if (code == -1) return;                 // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(code, expectedExitCode);  // no UAF / double-free
}

// fix #3851/#3892: thin local forwarder kept for the unqualified call sites
// below (lines that pin specific rejection message text unrelated to the
// runtime-vs-compile-time discriminator) — the ONE implementation now lives
// in tests/support/aot_result_asserts.hpp (see that header's comment for the
// full, verified breakdown of why it is a differently-named function rather
// than a same-named one: exactly one other file
// — tests/codegen/encoding_getbytecount_span_1355_stdlib_3_2_test.cpp — has
// the byte-identical `hasErrorContaining(const bezel::test::JitResult&,
// const char*)` signature ADL would make ambiguous; 7 more files share the
// name under a non-colliding or differently-typed signature, for 8 across
// the whole family — not the "~14" an earlier revision of this comment
// claimed).
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    return bezel::test::diagnosticMessageContains(r, sub);
}

// fix #3889 (review finding) / fix #3892 (promoted to shared support): see
// tests/support/aot_result_asserts.hpp's `failedAtCompileTimeNotRuntime` for
// the full rationale — `r.ran == false` alone does NOT distinguish a genuine
// compile-time rejection from a runtime crash/signal/timeout/ASan report.
using bezel::test::failedAtCompileTimeNotRuntime;

// fix #3851: the diagnostic must point at the call, never at 0:0. A REJECT's
// `runAot` packs the whole captured stderr into ONE synthetic Diagnostic at
// SourceLocation{} (0:0) — `d.location` itself cannot be asserted — so this
// parses the real `<file>:<line>:<col>:` prefix rendered ahead of `needle`
// inside the message text (mirrors tests/sema/string_isnullorempty_removed_
// 3854_test.cpp's `locatedAt`).
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

// === GetBytes: byte values (§3.2) ======================================

// §3.2: Encoding.UTF8.GetBytes("Hello") yields the 5 ASCII bytes H,e,l,l,o.
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  return b.Count; "
        "end Main; end C;", 5);
}

// #984 / §3.2: an INVALID Encoding singleton name (e.g. `Encoding.Utf8` — the
// old PascalCase form; §3.2 v0.148 renamed the singletons to UTF8/UTF16/UTF32/ASCII)
// must be rejected with a clear diagnostic, not left untyped to silently
// miscompile (the chained `.GetBytes(...)` had no codegen lowering →
// uninitialized local → Length 0 / SIGSEGV on index).
BEZEL_TEST(codegen_encoding, invalid_singleton_name_rejected_984) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var b: Array<byte> := Encoding.Utf8.GetBytes(\"hi\"); "
        "  return b.Count; "
        "end Main; end C;"));
}

// #984: the valid `Encoding.UTF8` form (all-caps, §3.2 v0.148) compiles fine —
// the reject must not be over-broad (guards against rejecting a legitimate singleton).
BEZEL_TEST(codegen_encoding, valid_singleton_name_accepted_984) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(\"hi\"); "
        "  return b.Count; "
        "end Main; end C;", 2);
}

// §3.2: the byte at index 0 of GetBytes("Hello") is the ASCII code of 'H' (72).
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_byte_value_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  return int(b.Get(0)); "
        "end Main; end C;", 72);
}

// §3.2: every ASCII byte of GetBytes("ABC") matches the ASCII codes (65,66,67).
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_all_bytes_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  var ok := b.Get(0) = byte(65) and b.Get(1) = byte(66) and b.Get(2) = byte(67); "
        "  return (if ok then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2: the empty string yields an empty byte array.
BEZEL_TEST(codegen_encoding, utf8_getbytes_empty_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  return b.Count; "
        "end Main; end C;", 0);
}

// §3.2: a non-ASCII code point (é = U+00E9) UTF-8-encodes to two bytes
// (0xC3 0xA9 = 195, 169), so "café" (4 code points) yields 5 bytes.
BEZEL_TEST(codegen_encoding, utf8_getbytes_multibyte_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  return b.Count; "
        "end Main; end C;", 5);
}

// §3.2: the two continuation bytes of é (U+00E9) are 0xC3 (195) and 0xA9 (169).
BEZEL_TEST(codegen_encoding, utf8_getbytes_multibyte_bytes_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  var ok := b.Get(3) = byte(195) and b.Get(4) = byte(169); "
        "  return (if ok then 1 else 0); "
        "end Main; end C;", 1);
}

// === GetString and round-trip (§3.2) ===================================

// §3.2: GetString(GetBytes(s)) == s for an ASCII string (lossless round-trip).
BEZEL_TEST(codegen_encoding, utf8_roundtrip_ascii_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello, World\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var back: string := Encoding.UTF8.GetString(b); "
        "  return (if back = s then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2: the round-tripped string has the same code-point length as the source.
BEZEL_TEST(codegen_encoding, utf8_roundtrip_ascii_length_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var back: string := Encoding.UTF8.GetString(b); "
        "  return back.Length; "
        "end Main; end C;", 5);
}

// §3.2: GetString(GetBytes(s)) == s for a multi-byte UTF-8 string (é = U+00E9).
BEZEL_TEST(codegen_encoding, utf8_roundtrip_multibyte_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9 \\u00fcber\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var back: string := Encoding.UTF8.GetString(b); "
        "  return (if back = s then 1 else 0); "
        "end Main; end C;", 1);
}

// fix #3851 (stdlib §3.2 / §1, v0.290, proposal 0123 R5 Option B): the
// three-argument `GetString(bytes, index, count)` window call form this row
// used to accept (decoding the sub-range [index, index+count),
// GetString(GetBytes("Hello"), 1, 3) == "ell") was retired — a window over
// an Array<byte> is a slice, not a parameter pair (§4.14's no-offset-
// parameter rule). The call must now be REJECTED, naming the real
// replacement, at the real call-site location (never 0:0).
BEZEL_TEST(codegen_encoding, utf8_getstring_index_count_call_form_retired_3851) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var sub: string := Encoding.UTF8.GetString(b, 1, 3); "
        "  return (if sub = \"ell\" and sub.Length = 3 then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "was REMOVED from the standard library at v0.290"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "GetString(bytes.AsReadOnlySpan(start, length))"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "issue 3851"));
    BEZEL_EXPECT_TRUE(hasErrorLocatedAt(
        r, "the three-argument `Encoding.GetString(bytes, start, length)`"));
}

// §3.2: GetString of an empty byte array is the empty string.
BEZEL_TEST(codegen_encoding, utf8_getstring_empty_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var back: string := Encoding.UTF8.GetString(b); "
        "  return back.Length; "
        "end Main; end C;", 0);
}

// === UTF8 strict well-formedness (fix #1144, stdlib §3.2 v0.136) =======
//
// §3.2: "All four standard encodings follow one error model: malformed or
// unrepresentable input is a defined fault, never a silent substitution."
// For UTF8, GetString must raise FormatException on ill-formed sequences —
// overlong forms, unpaired-surrogate encodings, truncated tails (and, by the
// same model, invalid lead bytes, lone continuation bytes, and code points
// above U+10FFFF). Before #1144 the decoder was lenient: it passed an
// ill-formed byte through as a literal code point (e.g. a lone 0x80 decoded to
// U+0080), silently accepting input the spec mandates be rejected.

// §3.2 / #1144: a lone continuation byte (0x80) is ill-formed → FormatException.
// This is the issue's exact repro (it previously returned 128, no exception).
BEZEL_TEST(codegen_encoding, utf8_getstring_lone_continuation_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0x80)); "  // lone UTF-8 continuation byte
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: an overlong 2-byte form (C0 AF = overlong U+002F) → FormatException.
BEZEL_TEST(codegen_encoding, utf8_getstring_overlong_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xC0)); b.Add(byte(0xAF)); "  // overlong encoding of '/'
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: a surrogate encoded in UTF-8 (ED A0 80 = U+D800) → FormatException.
BEZEL_TEST(codegen_encoding, utf8_getstring_surrogate_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xED)); b.Add(byte(0xA0)); b.Add(byte(0x80)); "  // U+D800
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: a truncated 3-byte tail (E2 82, missing 3rd byte) → FormatException.
BEZEL_TEST(codegen_encoding, utf8_getstring_truncated_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xE2)); b.Add(byte(0x82)); "  // truncated 3-byte sequence
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: a code point above U+10FFFF (F4 90 80 80 = U+110000) → FormatException.
BEZEL_TEST(codegen_encoding, utf8_getstring_above_max_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xF4)); b.Add(byte(0x90)); b.Add(byte(0x80)); b.Add(byte(0x80)); "
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: an invalid lead byte (0xFF) → FormatException.
BEZEL_TEST(codegen_encoding, utf8_getstring_invalid_lead_raises_format_1144) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xFF)); "  // 0xFF is never a valid UTF-8 byte
        "  try "
        "    using var s: string := Encoding.UTF8.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 / #1144: the strict check must NOT reject well-formed multi-byte input.
// Raw bytes: C3 A9 (é = U+00E9), EF BB BF (BOM, decoded to U+FEFF — NOT stripped,
// per §3.2 "GetString does not treat a leading EF BB BF specially"), and
// F0 9F 98 80 (U+1F600, a 4-byte sequence) all decode to their code points.
BEZEL_TEST(codegen_encoding, utf8_getstring_wellformed_multibyte_decodes_1144) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0xC3)); b.Add(byte(0xA9)); "                 // é = U+00E9
        "  b.Add(byte(0xEF)); b.Add(byte(0xBB)); b.Add(byte(0xBF)); "  // BOM → U+FEFF
        "  b.Add(byte(0xF0)); b.Add(byte(0x9F)); b.Add(byte(0x98)); b.Add(byte(0x80)); "  // U+1F600
        "  using var s: string := Encoding.UTF8.GetString(b); "
        "  var ok := s.Length = 3 and int(s[0]) = 233 and "
        "             int(s[1]) = 65279 and int(s[2]) = 128512; "
        "  return (if ok then 1 else 0); "
        "end Main; end C;", 1);
}

// === Deferred singletons rejected (§3.2 / CLAUDE.md §4) =================

// ===========================================================================
// #553 — Encoding.UTF16 / Encoding.UTF32 / Encoding.ASCII (stdlib §3.2 v0.136)
// ===========================================================================

// ---------------------------------------------------------------------------
// UTF16 (UTF-16LE, no BOM emitted, stdlib §3.2 v0.136)
// ---------------------------------------------------------------------------

// §3.2 v0.136: UTF16.GetBytes("AB") — 'A' is U+0041, 'B' is U+0042.
// UTF-16LE encodes BMP chars as 2 bytes LE: 'A' → 0x41,0x00 ; 'B' → 0x42,0x00.
// Total: 4 bytes; first byte of 'A' is 0x41 (65).
BEZEL_TEST(codegen_encoding, utf16_getbytes_ascii_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF16.GetBytes(\"AB\"); "
        "  return b.Length; "
        "end Main; end C;", 4);
}

// §3.2 v0.136: UTF16.GetBytes("A") → byte[0]=0x41 (first byte of LE unit 0x0041).
BEZEL_TEST(codegen_encoding, utf16_getbytes_first_byte_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF16.GetBytes(\"A\"); "
        "  return int(b.Get(0)); "
        "end Main; end C;", 65);
}

// §3.2 v0.136: UTF16.GetBytes("A") → byte[1]=0x00 (high byte of LE unit 0x0041).
BEZEL_TEST(codegen_encoding, utf16_getbytes_second_byte_zero_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF16.GetBytes(\"A\"); "
        "  return int(b.Get(1)); "
        "end Main; end C;", 0);
}

// §3.2 v0.136: UTF16.GetString round-trip for an ASCII string (no BOM).
BEZEL_TEST(codegen_encoding, utf16_roundtrip_ascii_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF16.GetBytes(\"Hello\"); "
        "  using var back: string := Encoding.UTF16.GetString(b); "
        "  return (if back = \"Hello\" then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF16.GetBytes does NOT emit a BOM — byte[0] for 'H' is 0x48, not 0xFF.
BEZEL_TEST(codegen_encoding, utf16_getbytes_no_bom_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF16.GetBytes(\"H\"); "
        "  return (if int(b.Get(0)) = 72 and b.Length = 2 then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF16.GetString consumes one leading LE BOM (FF FE) when present.
// The output string should NOT contain U+FEFF; round-trip is "Hi" not "﻿Hi".
BEZEL_TEST(codegen_encoding, utf16_getstring_consume_bom_spec_3_2) {
    // Manually build bytes: FF FE 48 00 69 00 (BOM + "Hi" in UTF-16LE).
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(255)); b.Add(byte(254)); "  // FF FE = LE BOM
        "  b.Add(byte(72)); b.Add(byte(0)); "     // 'H'
        "  b.Add(byte(105)); b.Add(byte(0)); "    // 'i'
        "  using var s: string := Encoding.UTF16.GetString(b); "
        "  return (if s = \"Hi\" and s.Length = 2 then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF16.GetString raises FormatException on odd byte count.
BEZEL_TEST(codegen_encoding, utf16_getstring_odd_byte_count_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(65)); "  // 1 byte — odd count
        "  try "
        "    using var s: string := Encoding.UTF16.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: UTF16.GetString raises FormatException on a big-endian BOM (FE FF).
BEZEL_TEST(codegen_encoding, utf16_getstring_big_endian_bom_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(254)); b.Add(byte(255)); "  // FE FF = BE BOM
        "  b.Add(byte(72)); b.Add(byte(0)); "
        "  try "
        "    using var s: string := Encoding.UTF16.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: UTF16.GetString raises FormatException on an unpaired surrogate.
BEZEL_TEST(codegen_encoding, utf16_getstring_unpaired_surrogate_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0x00)); b.Add(byte(0xD8)); "  // 0xD800 = lone high surrogate
        "  try "
        "    using var s: string := Encoding.UTF16.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: UTF16.Name == "utf-16le".
BEZEL_TEST(codegen_encoding, utf16_name_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var n: string := Encoding.UTF16.Name; "
        "  return (if n = \"utf-16le\" then 1 else 0); "
        "end Main; end C;", 1);
}

// ---------------------------------------------------------------------------
// UTF32 (UTF-32LE, no BOM emitted, stdlib §3.2 v0.136)
// ---------------------------------------------------------------------------

// §3.2 v0.136: UTF32.GetBytes("A") — 'A' (U+0041) → 4 bytes LE: 0x41 0x00 0x00 0x00.
BEZEL_TEST(codegen_encoding, utf32_getbytes_ascii_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF32.GetBytes(\"A\"); "
        "  return b.Length; "
        "end Main; end C;", 4);
}

// §3.2 v0.136: UTF32.GetBytes("A") → byte[0]=0x41, byte[3]=0x00.
BEZEL_TEST(codegen_encoding, utf32_getbytes_first_byte_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF32.GetBytes(\"A\"); "
        "  var ok := int(b.Get(0)) = 65 and int(b.Get(1)) = 0 and "
        "             int(b.Get(2)) = 0 and int(b.Get(3)) = 0; "
        "  return (if ok then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF32round-trip for an ASCII string.
BEZEL_TEST(codegen_encoding, utf32_roundtrip_ascii_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF32.GetBytes(\"Hello\"); "
        "  using var back: string := Encoding.UTF32.GetString(b); "
        "  return (if back = \"Hello\" then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF32.GetBytes does NOT emit a BOM.
BEZEL_TEST(codegen_encoding, utf32_getbytes_no_bom_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.UTF32.GetBytes(\"A\"); "
        "  return (if int(b.Get(0)) = 65 and b.Length = 4 then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF32.GetString raises FormatException on non-divisible-by-4 count.
BEZEL_TEST(codegen_encoding, utf32_getstring_non_div4_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(65)); b.Add(byte(0)); b.Add(byte(0)); "  // 3 bytes
        "  try "
        "    using var s: string := Encoding.UTF32.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: UTF32.GetString raises FormatException on a surrogate code unit.
BEZEL_TEST(codegen_encoding, utf32_getstring_surrogate_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(0x00)); b.Add(byte(0xD8)); b.Add(byte(0)); b.Add(byte(0)); "
        "  try "
        "    using var s: string := Encoding.UTF32.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: UTF32.Name == "utf-32le".
BEZEL_TEST(codegen_encoding, utf32_name_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var n: string := Encoding.UTF32.Name; "
        "  return (if n = \"utf-32le\" then 1 else 0); "
        "end Main; end C;", 1);
}

// ---------------------------------------------------------------------------
// ASCII (strict 7-bit, stdlib §3.2 v0.136)
// ---------------------------------------------------------------------------

// §3.2 v0.136: ASCII.GetBytes("Hi") → 2 bytes: 0x48 ('H') and 0x69 ('i').
BEZEL_TEST(codegen_encoding, ascii_getbytes_ascii_string_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.ASCII.GetBytes(\"Hi\"); "
        "  var ok := b.Length = 2 and int(b.Get(0)) = 72 and int(b.Get(1)) = 105; "
        "  return (if ok then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: ASCII round-trip.
BEZEL_TEST(codegen_encoding, ascii_roundtrip_ascii_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := Encoding.ASCII.GetBytes(\"Hello\"); "
        "  using var back: string := Encoding.ASCII.GetString(b); "
        "  return (if back = \"Hello\" then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: ASCII.GetBytes raises FormatException on any code point > U+007F.
BEZEL_TEST(codegen_encoding, ascii_getbytes_non_ascii_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    using var b: Array<byte> := Encoding.ASCII.GetBytes(\"caf\\u00e9\"); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: ASCII.GetString raises FormatException on any byte > 0x7F.
BEZEL_TEST(codegen_encoding, ascii_getstring_high_byte_raises_format_spec_3_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var b: Array<byte> := new Array<byte>(); "
        "  b.Add(byte(65)); b.Add(byte(200)); "  // 200 > 0x7F
        "  try "
        "    using var s: string := Encoding.ASCII.GetString(b); "
        "    return 0; "
        "  catch FormatException => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §3.2 v0.136: ASCII.Name == "us-ascii".
BEZEL_TEST(codegen_encoding, ascii_name_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var n: string := Encoding.ASCII.Name; "
        "  return (if n = \"us-ascii\" then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2 v0.136: UTF8.Name == "utf-8" (regression guard, name property sanity).
BEZEL_TEST(codegen_encoding, utf8_name_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var n: string := Encoding.UTF8.Name; "
        "  return (if n = \"utf-8\" then 1 else 0); "
        "end Main; end C;", 1);
}

// §3.2: an `Encoding` value is reachable ONLY as the immediate receiver of a
// direct `Encoding.UTF8.GetBytes/GetString` call — there is no first-class
// singleton object yet. Binding `Encoding.UTF8` to a local and calling through
// it must fail to compile (a clear decline-with-reasons, not a miscompile).
BEZEL_TEST(codegen_encoding, indirect_singleton_use_rejected_spec_3_2) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var e := Encoding.UTF8; "
        "  var s := \"Hi\"; "
        "  using var b: Array<byte> := e.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;"));
}

// === span-source GetString overloads (fix #3889, stdlib §3.2 v0.222 #1809) ==
//
// GetString(bytes: ReadOnlyArraySpan<byte>) and GetString(bytes: ReadOnlySpan
// <byte>) — the two span-typed 1-argument overloads stdlib §3.2 documents
// alongside GetString(bytes: Array<byte>) — crashed codegen with an LLVM IR
// verification failure: tryLowerEncodingCall's 1-arg GetString arm
// (LlvmBackend_Calls2.cpp) always treated `call.args[0].value` as a raw
// bz_array_t* regardless of its static type, so a span-struct argument
// (%bz.arrayspan) was passed directly to a runtime entry point declared to
// take a bare pointer — the exact crash fix #3851's own retirement diagnostic
// steers users into by naming `GetString(bytes.AsReadOnlySpan(start, length))`
// as the replacement for the removed 3-argument window call. Fixed by
// detecting a struct-typed (span) lowered argument and unpacking { source,
// start, length } the same way the adjacent 2-arg GetBytes(s, destination)
// arm already does.
//
// Every row below asserts the DECODED VALUE (not just "it compiled"), and the
// windowed rows use a non-zero `start` so a fix that silently ignored the
// span's start offset (always decoding from 0) could not pass.

// fix #3889: the issue's own repro — Array<byte>.AsReadOnlySpan(1, 3) over
// GetBytes("Hello") (bytes 72,101,108,108,111) must decode exactly "ell",
// proving the windowed ReadOnlyArraySpan<byte> overload both compiles AND
// honors the non-zero start.
BEZEL_TEST(codegen_encoding,
           utf8_getstring_readonlyarrayspan_windowed_value_3889) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  var sub: string := Encoding.UTF8.GetString(b.AsReadOnlySpan(1, 3)); "
        "  return (if sub = \"ell\" and sub.Length = 3 then 1 else 0); "
        "end Main; end C;", 1);
}

// fix #3889: the no-offset ReadOnlyArraySpan<byte> form (b.AsReadOnlySpan(),
// start=0, length=Count) must still decode the FULL string — a regression
// guard that the span arm's zero-start case did not somehow drop bytes.
BEZEL_TEST(codegen_encoding, utf8_getstring_readonlyarrayspan_full_3889) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  var back: string := Encoding.UTF8.GetString(b.AsReadOnlySpan()); "
        "  return (if back = s and back.Length = 5 then 1 else 0); "
        "end Main; end C;", 1);
}

// fix #3889: a multi-byte-safe window. "café" UTF-8-encodes to bytes
// [99,97,102,195,169] (c,a,f, then é's two-byte 0xC3 0xA9 — see
// utf8_getbytes_multibyte_bytes_spec_3_2 above). The window [3, 2) covers
// exactly the two continuation bytes of é and must decode to "é" alone —
// guards against an off-by-one in the unpacked start/length beyond plain
// ASCII single-byte code points.
BEZEL_TEST(codegen_encoding,
           utf8_getstring_readonlyarrayspan_multibyte_window_3889) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  var sub: string := Encoding.UTF8.GetString(b.AsReadOnlySpan(3, 2)); "
        "  return (if sub = \"\\u00e9\" and sub.Length = 1 then 1 else 0); "
        "end Main; end C;", 1);
}

// fix #3889: the OwningBuffer<byte>-backed `ReadOnlySpan<byte>` overload — the
// sibling span kind stdlib §3.2 documents alongside ReadOnlyArraySpan<byte>
// (both share codegen's %bz.arrayspan layout per LlvmBackend_impl.hpp
// isSpanLikeTypeName; OwningBuffer<T> shares Array<T>'s bz_array_t* backing).
// Copies GetBytes("Hello")'s bytes into an OwningBuffer<byte> and decodes the
// windowed [1, 3) slice through AsReadOnlySpan(start, length) — must also
// yield exactly "ell", proving the non-Array span source unpacks identically.
BEZEL_TEST(codegen_encoding,
           utf8_getstring_readonlyspan_ownedbuffer_windowed_value_3889) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.UTF8.GetBytes(s); "
        "  using var buf: OwningBuffer<byte> := "
        "      new OwningBuffer<byte>(5, lambda(i: int) => b[i]); "
        "  var sub: string := "
        "      Encoding.UTF8.GetString(buf.AsReadOnlySpan(1, 3)); "
        "  return (if sub = \"ell\" and sub.Length = 3 then 1 else 0); "
        "end Main; end C;", 1);
}

// === negative controls: a struct-typed argument that is NOT a recognized
// span must never reach the span-unpack path (fix #3889 review finding) ====
//
// Sema's 1-arg GetString typing (ExprTypecheck_Calls.cpp ~5468) types ANY
// 1-arg call on an Encoding receiver as `string`, gated only on member name
// and arity — it does not itself check the argument's type against the three
// documented overloads (Array<byte> / ReadOnlySpan<byte> /
// ReadOnlyArraySpan<byte>). A discriminator keyed on the LOWERED LLVM shape
// alone (`arg->getType()->isStructTy()`) is therefore unsound: `Range<int>`
// and any record ALSO lower to an LLVM struct, and reusing
// spanSource/spanStart/spanLength on one reinterprets its raw bytes as
// `{ptr, i32, i32}` — measured to compile clean and then SIGSEGV at runtime
// (`Encoding.UTF8.GetString(1..5)`), a loud-diagnostic-into-quiet-crash
// inversion strictly worse than the pre-fix IR-verification failure. Both
// rows below must compile-FAIL the same way the pre-#3889 code always
// handled a non-Array<byte>, non-span argument here (an unimplemented-shape
// codegen error) — never produce a runnable binary.

// fix #3889 (review finding): Range<int> is a struct-typed argument that is
// NOT one of the three documented GetString overloads. Must fail AT COMPILE
// TIME (the real bezelc codegen pipeline, not sema alone — sema's
// arity/name-only gate accepts this call; only codegen's struct-vs-span
// discriminator can catch it) — never compile-and-segfault. Asserts the
// STRONG claim (failedAtCompileTimeNotRuntime), not merely `!r.ran`: a
// runtime SIGSEGV also has `r.ran == false` and would let a regressed
// discriminator pass this row for the wrong reason (exactly the review
// finding — see the helper's comment above).
BEZEL_TEST(codegen_encoding,
           utf8_getstring_range_argument_rejected_not_miscompiled_3889) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var sub: string := Encoding.UTF8.GetString(1..5); "
        "  return sub.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}

// fix #3889 (review finding): an arbitrary record is also a struct-typed
// argument that is NOT a recognized span. Same requirement: must fail AT
// COMPILE TIME, never compile-and-segfault via a reinterpreted-bytes span
// unpack (see failedAtCompileTimeNotRuntime's comment for why `!r.ran` alone
// is not a strong enough assertion here).
BEZEL_TEST(codegen_encoding,
           utf8_getstring_record_argument_rejected_not_miscompiled_3889) {
    auto r = runAot(
        "module M; record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(1, 2); "
        "  var sub: string := Encoding.UTF8.GetString(p); "
        "  return sub.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}

}  // namespace
