// fix #7999 (stdlib §3.2, §4.14, §4.15, §5.1, v0.350) — the §5.1 ADMISSION
// of `Encoding.GetBytes(s, destination)`.
//
// Authority: stdlib §3.2 v0.350, verbatim:
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//                                                                              // returns bytes written;
//                                                                              //   IndexOutOfRangeException if destination too short
// so the SECOND parameter is a `Span<byte>` — a span over `byte`: stdlib
// §4.14's `Span<T>` / `ReadOnlySpan<T>` (over an `OwningBuffer<T>`) and
// stdlib §4.15's `ArraySpan<T>` / `ReadOnlyArraySpan<T>` (over an
// `Array<T>`).  stdlib §3.2 also declares the span-SOURCE overload
// `GetBytes(s: ReadOnlyStringSpan, destination: Span<byte>): int`, so the
// FIRST position declares `string` OR `ReadOnlyStringSpan`.
//
// THE DEFECT. The `Encoding` rows of the ONE built-in parameter table
// (`src/common/BuiltinRegistry.cpp`) carried NO declared type at the
// `destination` position, so the §5.1 admission gate had nothing to say
// there: an `Array<byte>`, a `string`, an `int` — and even a `Span<char>`
// (a span over the WRONG element) — were all ADMITTED at the type level.
//
// MEASURED on the base (8993d643), with this file's own probes:
//   * `Encoding.UTF8.GetBytes("ab", buf)` over an `Array<byte>` was refused
//     by the CODEGEN arm (fix #7603) and NOT by sema;
//   * `Encoding.UTF8.GetBytes("ab", cbuf.AsSpan())` over a `Span<char>` was
//     ADMITTED CLEANLY — the codegen arm tests only that the argument is
//     SPAN-LIKE, never that the span is over `byte` — and the encoded bytes
//     were written into a `char` buffer.
//   * `Encoding.UTF8.GetBytes("ab", buf.AsReadOnlySpan())` over a
//     `ReadOnlySpan<byte>` was ADMITTED (the byte-span family the corpus
//     already admits; this fix does not narrow it).
//
// The MEMORY-SAFETY half of the original defect (a write through a garbage
// pointer) was closed by fix #7603 in the codegen arm.  This file pins the
// SEMANTIC half: the refusal is a §5.1 one, made where the DECLARED type is
// checked, and it says so — the diagnostic names §5.1's conversion rule, and
// the codegen arm's own marker is ABSENT from it.
//
// Bezel syntax notes: a `byte` prints through `int(...)`; an uncaught runtime
// raise would exit 70 the whole binary, so a rejected program is pinned
// through `runAot`'s `ran` flag and its diagnostics.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff some diagnostic contains `sub`.
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §3.2 destination rule, as the diagnostic spells it.  The wording is
// asserted, not merely "some error": a rejection that fires for an unrelated
// reason would satisfy a bare `ran == false` check.
const char* kDestDiag =
    "`Encoding.GetBytes` expects a `Span<byte>` destination (stdlib \xC2\xA7""3.2)";

}  // namespace

// ── The defect: a destination that is not a byte span is REFUSED ────────────

// The `Array<byte>` destination, in the NAMED spelling.  On the base this was
// refused by the CODEGEN arm (fix #7603) — whose message names the §3.2 rule
// but NOT §5.1 — so the row is RED on the base and GREEN with the fix.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_destination_must_be_a_byte_span) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\", destination := buf); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `Array<byte>`"));
    // The §5.1 REASON — the codegen arm cannot state it (it is a LOWERING
    // refusal, not a conversion one), so this assertion is what makes the row
    // RED on the base: there the refusal is the codegen arm's, and it does
    // not carry §5.1.
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""5.1"));
    // ... and the codegen arm's own marker is ABSENT: the refusal is the
    // SEMANTIC one.
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, "(#7603)"));
}

// A `string` destination: stdlib §3.2 declares a `Span<byte>` there, and
// §5.1's table has no conversion from a `string` to a byte span.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_destination_string_is_refused) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", \"xy\"); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `string`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""5.1"));
}

// A span over the WRONG element: `Span<char>` is a span, but not a byte
// span, and stdlib §3.2 declares `Span<byte>`.  On the base this was
// ADMITTED (the codegen arm tests only that the argument is span-LIKE), and
// the encoded bytes were written into a `char` buffer.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_destination_wrong_element_span_is_refused) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var cbuf := new OwningBuffer<char>(8); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", cbuf.AsSpan()); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `Span<char>`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""5.1"));
}

// The POSITIONAL spelling refuses the same way: the defect is the
// destination's TYPE, not the spelling.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_positional_destination_is_refused) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\", buf); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `Array<byte>`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""5.1"));
}

// The REORDERED named spelling refuses the same way: §22.5 binds the named
// argument to the parameter it NAMES, so `destination :=` is the destination
// whichever position it is written at.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_reordered_destination_is_refused) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(destination := buf, s := \"ab\"); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `Array<byte>`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""5.1"));
}

// ── Controls: the legal byte spans still work ───────────────────────────────

// The LEGAL byte span — stdlib §4.14's `Span<byte>`, the view an
// `OwningBuffer<byte>` hands out — still compiles, runs, and writes the
// encoded bytes.  This row is GREEN on the base and GREEN with the fix: it
// is the admitting twin of the refusals above, so it shows the refusals fire
// on the TYPE and not on the shape of the call.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_byte_span_destination_is_accepted) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\", destination := buf.AsSpan()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(buf[0])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "ab" is two UTF-8 bytes; the first is 'a' = 97.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n97\n"));
}

// The `ArraySpan<byte>` spelling the PINNED suite
// (`encoding_getbytecount_span_1355`) uses — stdlib §4.15's view over an
// `Array<T>`, what `Array<byte>.AsSpan()` returns — is STILL accepted.  This
// is the guard the #7603 review recorded as F6: the compiler admits
// `ArraySpan<byte>` at stdlib §3.2's `Span<byte>` parameter, and §5.1's
// table has no span-to-span conversion, so the question of whether it
// SHOULD be admitted is not this fix's to decide.  What this fix decides is
// that the admission is a TYPE admission: a byte span is admitted, and
// everything else is not.
BEZEL_TEST(encoding_getbytes_destination_span_7999,
           getbytes_arrayspan_destination_is_still_accepted) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\", destination := buf.AsSpan()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(buf[0])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n97\n"));
}
