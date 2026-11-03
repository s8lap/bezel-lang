// fix #8027 (stdlib §3.2, §4.14, §4.15, §5.1, v0.350) — `Encoding.GetBytes(s,
// destination)` accepted a READ-ONLY span view at its WRITABLE destination and
// wrote THROUGH it.
//
// THE DEFECT. stdlib §3.2 (v0.350) declares, verbatim:
//
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//                                                                              // returns bytes written;
//                                                                              //   IndexOutOfRangeException if too short
//
// so the second parameter is a `Span<byte>` — a WRITABLE span: stdlib §4.14's
// `Span<T>` over an `OwningBuffer<T>`, or stdlib §4.15's `ArraySpan<T>` over
// an `Array<T>`.  stdlib §4.14 also declares `ReadOnlySpan<T>` and §4.15
// `ReadOnlyArraySpan<T>`: each is a READ-ONLY VIEW of the very memory the
// writable span writes.
//
// The codegen arm for this overload — `tryLowerEncodingCall` in
// `src/codegen/LlvmBackend_Calls2.cpp` — discriminates the destination with
// `isSpanLikeTypeName`, which admits the WHOLE span family
// (`Span`/`ReadOnlySpan`/`ArraySpan`/`ReadOnlyArraySpan` alike) and never asks
// whether the view is WRITABLE.  The four share the `bz.arrayspan` layout, so
// the arm's shape check cannot tell them apart either.  The encoded bytes
// were therefore written into the backing buffer BEHIND a read-only view.
//
// MEASURED on the base (48353f2b):
//   * `Encoding.UTF8.GetBytes("ab", buf.AsReadOnlySpan())` over a
//     `ReadOnlySpan<byte>` compiled CLEAN and RAN — the call returned the
//     byte count (2) and the bytes landed in the `OwningBuffer<byte>` the
//     read-only view reads;
//   * the same holds for `array.AsReadOnlySpan()` over an `Array<byte>`
//     (a `ReadOnlyArraySpan<byte>`).
//
// §5.1's conversion table has NO span-to-span conversion at all, so a
// `ReadOnlySpan<byte>` is not admissible at a `Span<byte>` parameter on the
// spec's own terms either: the correct answer is a REFUSAL, not a write.
//
// THE FIX is in the codegen arm — the same place fix #7603 closed the
// write-through of a destination that is not a span at all: the arm refuses a
// destination it cannot write through.  The SEMANTIC half (§5.1's admission
// gate, `isByteSpanType` in `src/sema/ExprTypecheck_Calls.cpp`) is a
// SEMANTIC-lane file and is NOT changed here.
//
// NOTHING PINNED LOSES: the conformance corpus and the suite use the WRITABLE
// forms (`.AsSpan()`) at this destination everywhere — stdlib §3.2's
// `GetBytes(s, destination)` rows (`l3_2_0010`, `l3_2_0011`) and the #7603 /
// #7999 suites all write through `dst.AsSpan()` / `big.AsSpan()`.
//
// Bezel syntax notes: a `byte` prints through `int(...)`; an uncaught runtime
// raise exits 70, so a rejected program is pinned through `runAot`'s `ran`
// flag and its diagnostics.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff some ERROR-severity diagnostic contains `sub`.
//
// The severity filter is load-bearing, not cosmetic.  `runAot` fills
// `r.diagnostics` ONLY on a compile/link reject (or a compiler crash/timeout, a
// sanitizer report, a signal) — every one of those paths returns BEFORE
// `r.ran` is set, so on a program that COMPILED the bag is EMPTY by
// construction (tests/support/aot_support.hpp).  On a REJECT the harness
// reports the compiler's whole stderr as ONE `Severity::Error` diagnostic, so a
// match here is against a REAL error, never against a Warning or a Note.
//
// Only the four REFUSAL rows use this helper, and they use it to assert that
// the refusal carried THIS text and THIS offending type: stdlib §3.2 declares
// `GetBytes(s: string, destination: Span<byte>)`, so the destination is a
// WRITABLE `Span<byte>` (§4.14's `Span<T>` over an `OwningBuffer<T>`, §4.15's
// `ArraySpan<T>` over an `Array<T>`), while §4.14's `ReadOnlySpan<T>` and
// §4.15's `ReadOnlyArraySpan<T>` are READ-ONLY views of the same memory, and
// §5.1's conversion table has no span-to-span conversion at all.
//
// The CONTROL rows use NO diagnostic assertion: their bag is empty by
// construction, so a negative over it is unobservable — the `check_vacuous_tests`
// ratchet is right about that (scripts/check_vacuous_tests.py).
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §3.2 destination rule, as the diagnostic spells it.  The wording is
// asserted, not merely "some error": a rejection that fires for an unrelated
// reason would satisfy a bare `ran == false` check.
const char* kDestDiag =
    "`Encoding.GetBytes` expects a `Span<byte>` destination (stdlib \xC2\xA7""3.2)";

}  // namespace

// ── The defect: a READ-ONLY span view is REFUSED at the writable destination ──

// The issue's own repro, over an `OwningBuffer<byte>` (§4.14's
// `ReadOnlySpan<T>`).  On the base this compiled clean and RAN (the call
// returned the byte count and the encoded bytes were written into the buffer
// behind the read-only view), so the row is RED on the base and GREEN with
// the fix.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           readonly_span_destination_is_refused_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsReadOnlySpan()); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    // The offending type is named, so the diagnostic says WHICH argument was
    // refused and not merely that something was.
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "ReadOnlySpan<byte>"));
}

// The §4.15 form: a `ReadOnlyArraySpan<byte>` over an `Array<byte>`.  Same
// defect, same refusal.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           readonly_array_span_destination_is_refused_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", arr.AsReadOnlySpan()); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "ReadOnlyArraySpan<byte>"));
}

// The §22.5 named spelling of the same destination — the §22.5 binder binds
// `destination` to the `destination` PARAMETER, so the refusal must fire on
// this spelling too (the issue's own `destination :=` form).
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           named_readonly_span_destination_is_refused_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\", "
        "                               destination := buf.AsReadOnlySpan()); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "ReadOnlySpan<byte>"));
}

// The REORDERED §22.5 spelling: the destination is written FIRST and the
// source second.  The refusal is the same refusal — the read-only view is
// still the destination.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           reordered_readonly_span_destination_is_refused_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(destination := buf.AsReadOnlySpan(), "
        "                               s := \"ab\"); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "ReadOnlySpan<byte>"));
}

// ── CONTROLS: the WRITABLE destinations are still admitted and still write ──
//
// Each control row asserts ONLY on the channels the harness really carries:
// the program RAN, it exited 0 (`Main` returns 0), and its stdout carries the
// encoded bytes.  It asserts NOTHING about `r.diagnostics`: that bag is EMPTY
// for a program that compiled (`runAot` fills it only on a reject), so a
// negative over it is unobservable and would only LOOK like coverage.  The
// observable form of "the WRITABLE destination was not refused" is the exit
// status plus the bytes: stdlib §3.2's `GetBytes(s, destination)` takes a
// WRITABLE `Span<byte>` (§4.14 / §4.15), so those destinations must still be
// admitted and must still write the encoded bytes.

// stdlib §4.14's `Span<T>` over an `OwningBuffer<T>` — the writable form the
// stdlib declares.  It must still be admitted, and it must still WRITE the
// encoded bytes into the buffer.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           writable_span_destination_still_writes_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsSpan()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(buf[0])); "
        "  Console.WriteLine(int(buf[1])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2\n97\n98\n") != std::string::npos);
}

// stdlib §4.15's `ArraySpan<T>` over an `Array<T>` — the other writable form.
// It must still be admitted and must still write.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           writable_array_span_destination_still_writes_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", arr.AsSpan()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(arr[0])); "
        "  Console.WriteLine(int(arr[1])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2\n97\n98\n") != std::string::npos);
}

// The ALLOCATING one-argument overload — `GetBytes(s): Array<byte>` — has no
// destination at all and must be untouched by this fix.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           allocating_getbytes_is_unaffected_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := Encoding.UTF8.GetBytes(\"ab\"); "
        "  Console.WriteLine(b.Count); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2\n") != std::string::npos);
}

// A READ-ONLY span is a perfectly good SOURCE: stdlib §3.2 declares
// `GetString(bytes: ReadOnlyStringSpan)`, and the byte-span views are the
// §4.14 / §4.15 spellings of the same memory.  The fix refuses a read-only
// view only where the stdlib declares a WRITABLE destination; the source
// position is untouched.
BEZEL_TEST(encoding_getbytes_readonly_destination_8027,
           readonly_span_is_still_a_valid_source_8027) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var b := Encoding.UTF8.GetBytes(\"hi\"); "
        "  var s := Encoding.UTF8.GetString(b.AsReadOnlySpan()); "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("hi\n") != std::string::npos);
}
