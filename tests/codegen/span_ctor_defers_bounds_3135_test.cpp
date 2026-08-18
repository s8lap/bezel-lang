// Codegen — issue #3135 / stdlib §4.14 "Construction patterns" (v0.195):
// THE FOUR ARRAY-BACKED SPAN DIRECT CONSTRUCTORS range-check EAGERLY, which
// the stdlib says they must not.
//
// stdlib §4.14 "Construction patterns" (v0.195), verbatim:
//
//   "The direct constructors of the five span records do **not** range-check
//    `(Start, Length)` against the source (v0.195): a mis-constructed span
//    faults at each out-of-range *access* instead — the span-level check
//    passes and the source's own bounds check throws `IndexOutOfRangeException`
//    (the §4.15 caveat describes the same composition). The `AsSpan`/
//    `AsReadOnlySpan` factories validate eagerly and are the recommended
//    construction path"
//
// The four array-backed ctors affected are
//   `new Span<T>(buf, start, length)`,
//   `new ReadOnlySpan<T>(buf, start, length)`,
//   `new ArraySpan<T>(source, start, length)`,
//   `new ReadOnlyArraySpan<T>(source, start, length)`.
//
// `ReadOnlyStringSpan` was already lowered without the eager check by the #3098
// arm, so this fix matches its neighbour (and the stdlib text).
//
// ── WHAT IT REPLACES (measured on eeed89e, the parent) ─────────────────────
//
// `lowerNew`'s span-ctor arm (src/codegen/LlvmBackend_Exprs2.cpp, #1275) emits
// two eager `emitGuardThrow` bounds checks against the backing `Count`:
//
//     // Start ∈ [0, Count]; Length ∈ [0, Count-Start].
//     emitGuardThrow(startOOB, "IndexOutOfRangeException");
//     emitGuardThrow(lenOOB,   "IndexOutOfRangeException");
//
// so `new ArraySpan<int>(arr, 0, 5)` over a 3-element array raises
// `IndexOutOfRangeException` AT CONSTRUCTION, where §4.14 says the construction
// succeeds and the fault is deferred to the first out-of-range *access*. A
// program that constructs a wide span and never indexes past the source — or
// that narrows it with `Slice` before any access — is rejected at runtime
// where the stdlib says it runs.
//
// ── THE REFERENCE BEHAVIOUR (per §4.14, v0.195) ────────────────────────────
//
// 1. The direct ctors DO NOT validate `(start, length)` against the source.
//    They succeed even when `start+length > source.Count` or `start < 0`.
// 2. The `AsSpan`/`AsReadOnlySpan` factories DO validate eagerly (and have
//    since #636 / #746 — not the target of this fix).
// 3. `Slice(r: Range<int>)` validates eagerly "like the `AsSpan(r)` factories
//    and unlike the direct constructors and the `(start, length)` overload".
// 4. Out-of-range ACCESS on a mis-constructed span throws
//    `IndexOutOfRangeException` (the §4.15 / §3.3 guard, post #1158 / #3098).
//
// ── ANTI-OVER-REACH ────────────────────────────────────────────────────────
//
// The eager-check tests in tests/codegen/array_span_549_test.cpp
// (`span_ctor_length_oob_throws_1275`, `span_ctor_start_oob_throws_1275`)
// *also* pass after the fix — the access path throws the same exception — but
// their comments cite the wrong reason. The test for the *throw site* (ctor
// vs access) is the one below; the existing tests are left to stand as
// positive controls.
//
// The `AsSpan`/`AsReadOnlySpan` and `Slice(r)` paths are unchanged; their
// existing tests in array_span_549_test.cpp / asspan_range_746_test.cpp remain
// the regression guard.

#include <string>

#include "support/aot_support.hpp"   // runAot — the AOT compile+run oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// runAot reports a toolchain SKIP as ran=false with a diagnostic containing
// "SKIP"; treat that as a no-op rather than a failure.
bool aotWasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string_view::npos) return true;
    return false;
}

// An `OwningBuffer<int>` of 5 filled 10..14 — the §4.14 source for the
// `Span<T>` / `ReadOnlySpan<T>` pair.
std::string buf5(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var buf := new OwningBuffer<int>(5); "
           "  buf[0] := 10; buf[1] := 11; buf[2] := 12; "
           "  buf[3] := 13; buf[4] := 14; "
           + body +
           "end Main; end C;";
}

// An `Array<int>` of 5 filled 20..24 — the §4.15 source for the
// `ArraySpan<T>` / `ReadOnlyArraySpan<T>` pair.
std::string arr5(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var arr := new Array<int>(5, 0); "
           "  arr[0] := 20; arr[1] := 21; arr[2] := 22; "
           "  arr[3] := 23; arr[4] := 24; "
           + body +
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §4.14 — direct ctors DO NOT range-check (start, length) against the source.
// ---------------------------------------------------------------------------
//
// The cleanest distinguish test: at HEAD the ctor throws, the catch returns 88.
// At FIX the ctor succeeds, the `sp.Length` returns the length we ASKED for,
// the catch is skipped, and the test exits with that length. The two exit codes
// differ, so a regression to the eager-check rule is a hard FAIL on this test.

// §4.14 — `new Span<T>(buf, start, length)` constructs silently with a length
// that overflows the source's Count. At HEAD the ctor throws and the catch
// returns 88; at FIX the ctor succeeds and the test exits with the Length.
//
// NOTE: exit codes are 8-bit, so we return a small Length (200) that fits and
// still overflows the 5-element source. (The fact that the spec example uses
// `1000000` is irrelevant to the throw-vs-not distinction; what matters is
// that `Start+Length` exceeds `Count`.)
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135, span_ctor_overlong_defers_check) {
    JitResult r = runAot(buf5(
        "  try "
        "    var sp := new Span<int>(buf, 0, 200); "
        "    return sp.Length; "            // 200 (FIX) — was 88 (HEAD)
        "  catch IndexOutOfRangeException => return 88; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// §4.14 — `new ReadOnlySpan<T>(buf, start, length)` likewise.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           readonlyspan_ctor_overlong_defers_check) {
    JitResult r = runAot(buf5(
        "  try "
        "    var sp := new ReadOnlySpan<int>(buf, 0, 200); "
        "    return sp.Length; "            // 200 (FIX) — was 88 (HEAD)
        "  catch IndexOutOfRangeException => return 88; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// §4.14 / §4.15 — `new ArraySpan<T>(arr, start, length)` likewise.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           arrayspan_ctor_overlong_defers_check) {
    JitResult r = runAot(arr5(
        "  try "
        "    var sp := new ArraySpan<int>(arr, 0, 200); "
        "    return sp.Length; "            // 200 (FIX) — was 88 (HEAD)
        "  catch IndexOutOfRangeException => return 88; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// §4.14 / §4.15 — `new ReadOnlyArraySpan<T>(arr, start, length)` likewise.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           readonlyarrayspan_ctor_overlong_defers_check) {
    JitResult r = runAot(arr5(
        "  try "
        "    var sp := new ReadOnlyArraySpan<int>(arr, 0, 200); "
        "    return sp.Length; "            // 200 (FIX) — was 88 (HEAD)
        "  catch IndexOutOfRangeException => return 88; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// §4.14 — a NEGATIVE `Start` likewise defers. (At HEAD both checks throw; at
// FIX the negative Start passes the ctor and the SOURCE check on first access
// — §4.15 / §3.3's unsigned compare — catches it.) `sp.Start + 100` is used
// to map the negative i32 Start (-1) to a positive i32 return value (99)
// that survives the 8-bit exit-code truncation. 88 is the catch return.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135, span_ctor_negative_start_defers_check) {
    JitResult r = runAot(buf5(
        "  try "
        "    var sp := new Span<int>(buf, -1, 5); "
        "    return sp.Start + 100; "       // 99 (FIX) — was 88 (HEAD)
        "  catch IndexOutOfRangeException => return 88; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ---------------------------------------------------------------------------
// §4.14 — out-of-range ACCESS on a mis-constructed span still throws
// `IndexOutOfRangeException` (the safety precondition of the no-eager-check
// rule). The four array-backed ctors + the access path mirror #3098's
// `ReadOnlyStringSpan` arm, which this same file's sister test asserts.
//
// This is the TEST that PINS the safety: every over-long construction must
// throw IndexOutOfRangeException at the first access that goes past the
// source. Both halves of a mis-range are covered:
//   * over-long `Length`  — the span check passes (`i < Length`), the source
//     check fires on `absIdx = Start+i`;
//   * negative `Start`    — the span check says nothing about `Start`, so only
//     the source check (unsigned compare, §4.15 / §3.3) can fire.
// ---------------------------------------------------------------------------

// §4.14 / §4.15 — `new Span<int>(buf, 0, 100)` then `sp[50]` throws. The
// span check `50 < 100` passes; the source check `Start+50 = 50 < Count=5`
// fails. IndexOutOfRangeException is caught.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           span_overlong_access_throws_3135) {
    JitResult r = runAot(buf5(
        "  var got: int := 0; "
        "  try "
        "    var sp := new Span<int>(buf, 0, 100); "
        "    var x := sp[50]; "
        "  catch IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  return got; "                    // 1 (FIX) — was 1 (HEAD, ctor threw)
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.14 / §4.15 — `new ReadOnlySpan<int>(buf, 0, 100)` then `sp[50]` throws.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           readonlyspan_overlong_access_throws_3135) {
    JitResult r = runAot(buf5(
        "  var got: int := 0; "
        "  try "
        "    var sp := new ReadOnlySpan<int>(buf, 0, 100); "
        "    var x := sp[50]; "
        "  catch IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  return got; "                    // 1
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.15 — `new ArraySpan<int>(arr, 0, 100)` then `sp[50]` throws.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           arrayspan_overlong_access_throws_3135) {
    JitResult r = runAot(arr5(
        "  var got: int := 0; "
        "  try "
        "    var sp := new ArraySpan<int>(arr, 0, 100); "
        "    var x := sp[50]; "
        "  catch IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  return got; "                    // 1
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.15 — `new ReadOnlyArraySpan<int>(arr, 0, 100)` then `sp[50]` throws.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           readonlyarrayspan_overlong_access_throws_3135) {
    JitResult r = runAot(arr5(
        "  var got: int := 0; "
        "  try "
        "    var sp := new ReadOnlyArraySpan<int>(arr, 0, 100); "
        "    var x := sp[50]; "
        "  catch IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  return got; "                    // 1
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §4.14 / §4.15 — a NEGATIVE Start likewise fires the source check, since the
// span check only guards `0 <= i < Length` (sign-extended `Start` is huge).
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           span_negative_start_access_throws_3135) {
    JitResult r = runAot(buf5(
        "  var got: int := 0; "
        "  try "
        "    var sp := new Span<int>(buf, -5, 10); "
        "    var x := sp[0]; "
        "  catch IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  return got; "                    // 1
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// §4.14 — REGRESSION GUARD for the AsSpan / AsReadOnlySpan factories.
//
// The factories are explicit in the stdlib text as the place eager
// validation LIVES: "The `AsSpan`/`AsReadOnlySpan` factories validate
// eagerly and are the recommended construction path". A fix that
// inadvertently broadens the no-eager-check rule to the factories would
// silently lose this safety net. Re-assert it here so a regression is a hard
// FAIL on this lane, not on a sister test's.
// ---------------------------------------------------------------------------

// §4.14 — `AsSpan(0, 100)` against a 5-element array STILL throws eagerly
// at the factory call site (NOT at the access — there is no access here).
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           asspan_factory_still_validates_eagerly) {
    JitResult r = runAot(arr5(
        "  try "
        "    var sp := arr.AsSpan(0, 100); "
        "    return 99; "                   // FIX and HEAD: never reached
        "  catch IndexOutOfRangeException => return 77; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// §4.14 — `AsReadOnlySpan(0, 100)` likewise still validates eagerly.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135,
           asreadonlyspan_factory_still_validates_eagerly) {
    JitResult r = runAot(arr5(
        "  try "
        "    var sp := arr.AsReadOnlySpan(0, 100); "
        "    return 99; "
        "  catch IndexOutOfRangeException => return 77; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// §4.14 — `AsSpan(0, 3)` is in-range: the factory returns a Length-3 span
// without throwing. Accept-control for the two above.
BEZEL_TEST(codegen_span_ctor_defers_bounds_3135, asspan_factory_in_range_ok) {
    JitResult r = runAot(arr5(
        "  var sp := arr.AsSpan(0, 3); "
        "  return sp.Length; "              // 3
        "  "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}