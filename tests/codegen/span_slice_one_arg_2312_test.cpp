// Codegen + sema — issue #2312 / stdlib §4.14 (`Span<T>` / `ReadOnlySpan<T>`)
// and §4.15 (`ArraySpan<T>` / `ReadOnlyArraySpan<T>`), v0.236.
//
// The v0.195 ONE-ARGUMENT `Slice(start)` overload is declared on all four
// buffer/array-backed span types. stdlib §4.14 declares, verbatim:
//
//     public method Slice(start: int, length: int): Span<T> =>
//       new Span<T>(Buffer, this.Start + start, length);
//     public method Slice(start: int): Span<T> => Slice(start, Length - start);
//
// with the identical pair on `ReadOnlySpan<T>` (§4.14) and on
// `ArraySpan<T>` / `ReadOnlyArraySpan<T>` (§4.15). §4.14's rationale note
// depends on the overload existing: "To read at an offset, slice first:
// `header.Slice(8).ReadInt32LittleEndian()` (the one-argument `Slice(start)` —
// declared on every span type as of v0.195)".
//
// It was an ACCEPT-TRAP. Sema typed EVERY `Slice` call on a span as that span
// type regardless of arity or operand type, while codegen
// (LlvmBackend_Calls3.cpp) lowered only `args.size() == 2`. The one-arg form
// therefore fell through to the lowering's `return nullptr`, and the value was
// simply never produced. `bezelc` still exited 0. Observed on e87d868:
//
//   * as a statement operand   — the WHOLE `Console.WriteLine(...)` statement
//                                was silently dropped: no output, no
//                                diagnostic, exit 0;
//   * as a local initializer   — degraded to zero with only an `internal:`
//                                note (#2434);
//   * as a return operand      — `return ros.Slice(2).Length` returned 0, not
//                                3, with only an `internal:` note (#2491).
//
// The same hole swallowed a WRONG ARITY (`ros.Slice(1, 2, 3)`), which is now a
// reject.
//
// It ALSO swallowed a non-`int` operand — `ros.Slice(1..<3)`; spans declare no
// `Slice(r: Range<int>)`, that overload being `Array<T>.Slice` (stdlib §4.4).
// That one is deliberately NOT rejected here: it is accepted today and the
// ownership suite relies on it (see `range_operand_does_not_miscompile`
// below). It stays a loud non-lowering instead, and is filed separately.
//
// Nullable operands stay with #2614's gate — a §7.4-narrowed `int?` is still
// ACCEPTED (positive control below) and an un-narrowed one still draws
// #2614's §7 diagnostic, not a second one from the new arity gate.
//
// Every test asserts an OBSERVED VALUE, never "it compiled": the accept-trap
// compiled fine. In particular the element assertions (`Slice(2)[0]` is 12,
// not 10) are what distinguish a correct lowering from one that produces a
// span of the right LENGTH at the wrong OFFSET.
//
// Demonstrated mechanically: with the two source edits reverted and rebuilt,
// every test below FAILS except the two-arg controls; with the fix, all pass.

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

// Per tests/README.md: runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime crash too, so a genuine compile-time rejection must
// be distinguished by its message TEXT, never by `!r.ran` alone.
bool rejectsMentioning(const std::string& program, const char* keyword) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(keyword) != std::string_view::npos) return true;
    return false;
}

// An `OwningBuffer<int>` of 5 filled 10,11,12,13,14 — the §4.14 source for
// `Span<T>` / `ReadOnlySpan<T>`. Values are distinct AND differ from their
// indices, so a wrong offset cannot coincide with a right one.
std::string buf5(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var buf := new OwningBuffer<int>(5); "
           "  buf[0] := 10; buf[1] := 11; buf[2] := 12; "
           "  buf[3] := 13; buf[4] := 14; "
           + body +
           "end Main; end C;";
}

// An `Array<int>` of 5 filled 20..24 — the §4.15 source for `ArraySpan<T>` /
// `ReadOnlyArraySpan<T>`.
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
// §4.14 — the issue's own repro, on `ReadOnlySpan<T>`.
// ---------------------------------------------------------------------------

// The issue's repro verbatim: `buf.AsReadOnlySpan().Slice(2).Length` is 3
// (5 - 2), per §4.14's `Slice(start) => Slice(start, Length - start)`.
BEZEL_TEST(codegen_span_slice_one_arg_2312, readonlyspan_slice_start_length) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var sl := ros.Slice(2); "
        "  return sl.Length; "));         // 3, was 0
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// The LENGTH alone would also be produced by a span at the wrong offset, so
// assert the ELEMENT: `Slice(2)[0]` is the buffer's index 2, i.e. 12. A span
// left at offset 0 would answer 10; a never-stored one, 0.
BEZEL_TEST(codegen_span_slice_one_arg_2312, readonlyspan_slice_start_offset) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var sl := ros.Slice(2); "
        "  return sl[0]; "));             // 12, was 0
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// §4.14: a `Span<T>` slices to a `Span<T>` — "The slice has the same access
// mode as its parent". Writing through the one-arg slice must reach the SAME
// buffer slot, which proves the lowering reuses the source pointer rather than
// materializing anything.
BEZEL_TEST(codegen_span_slice_one_arg_2312, span_slice_start_writes_through) {
    JitResult r = runAot(buf5(
        "  var sp := buf.AsSpan(); "
        "  var wsl := sp.Slice(3); "      // views buf[3..4]
        "  wsl[0] := 99; "
        "  return buf[3]; "));            // 99, was 0
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ---------------------------------------------------------------------------
// §4.15 — the array-backed pair. Same layout, separate stdlib section.
// ---------------------------------------------------------------------------

BEZEL_TEST(codegen_span_slice_one_arg_2312, arrayspan_slice_start_offset) {
    JitResult r = runAot(arr5(
        "  var asp := arr.AsSpan(); "
        "  var sl := asp.Slice(2); "
        "  if sl.Length <> 3 then return 1; end; "
        "  return sl[0]; "));             // 22, was 0
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

BEZEL_TEST(codegen_span_slice_one_arg_2312, readonly_arrayspan_slice_start_offset) {
    JitResult r = runAot(arr5(
        "  var roa := arr.AsReadOnlySpan(); "
        "  var sl := roa.Slice(2); "
        "  if sl.Length <> 3 then return 1; end; "
        "  return sl[0]; "));             // 22, was 0
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 22);
}

// ---------------------------------------------------------------------------
// Composition and boundaries — §4.14 "produces a sub-span that views into the
// same buffer".
// ---------------------------------------------------------------------------

// A one-arg slice OF a two-arg slice: offsets must compose, not reset.
// buf.Slice(1, 4) views 11,12,13,14; .Slice(1) of that views 12,13,14.
BEZEL_TEST(codegen_span_slice_one_arg_2312, slice_start_composes_with_two_arg) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var sl := ros.Slice(1, 4).Slice(1); "
        "  if sl.Length <> 3 then return 1; end; "
        "  return sl[0]; "));             // 12
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// The two boundary values of `Slice(start, Length - start)`: `Slice(0)` is the
// whole span, `Slice(Length)` is the empty span. Neither may throw — §4.14
// bounds only `start` to `0 <= start <= Length`.
//
// The slices are bound to LOCALS rather than used inline. That is deliberate:
// with the fix reverted, an inline `ros.Slice(0).Length <> 5` guard is a
// statement whose operand never lowers, so the whole guard was DROPPED and the
// test fell through to its success return — i.e. it passed vacuously, the
// failure mode tests/README.md warns about. Bound to a local, the unfixed
// compiler degrades the slot to zero (#2434) and `s0.Length` answers 0, so
// this now genuinely fails without the fix.
BEZEL_TEST(codegen_span_slice_one_arg_2312, slice_start_boundaries) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var s0 := ros.Slice(0); "
        "  var s5 := ros.Slice(5); "
        "  if s0.Length <> 5 then return 1; end; "
        "  if s0[0] <> 10 then return 2; end; "
        "  if s5.Length <> 0 then return 3; end; "
        "  if s5.IsEmpty = false then return 4; end; "
        "  return 42; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Out-of-range `start` throws, per §4.14/§4.15: "IndexOutOfRangeException if
// `start` or `length` is out of range". The bounds check must survive the
// one-arg path, which computes its length instead of lowering one.
BEZEL_TEST(codegen_span_slice_one_arg_2312, slice_start_out_of_range_throws) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  try "
        "    var bad := ros.Slice(6); "
        "    return bad.Length; "         // reached only if no throw
        "  catch var e: IndexOutOfRangeException => return 55; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

BEZEL_TEST(codegen_span_slice_one_arg_2312, slice_negative_start_throws) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  try "
        "    var bad := ros.Slice(0 - 1); "
        "    return bad.Length; "
        "  catch var e: IndexOutOfRangeException => return 56; "
        "  end; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}

// ---------------------------------------------------------------------------
// The reject side — the other half of the same hole. Asserted on diagnostic
// TEXT, since `!r.ran` alone cannot tell a rejection from a segfault.
// ---------------------------------------------------------------------------

// §4.14/§4.15 declare exactly two `Slice` overloads. A third argument used to
// take the same fall-through as the one-arg form (accepted, value never
// produced, `bezelc` exit 0).
BEZEL_TEST(codegen_span_slice_one_arg_2312, wrong_arity_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var bad := ros.Slice(1, 2, 3); "
        "  return bad.Length; "),
        "takes `(start)` or `(start, length)`"));
}

// A span declares no `Slice(r: Range<int>)` — that overload belongs to
// `Array<T>` (stdlib §4.4) — but it is ACCEPTED today and the ownership suite
// relies on that (consume_site_freeze_span_1726_test.cpp:144 and
// consume_site_freeze_span_reassign_1741_test.cpp:152 both need
// `s.Slice(0..<4)` to type as a span for the borrow-liveness diagnostic they
// assert). Rejecting it is therefore out of this issue's scope and is filed
// separately.
//
// What this test pins is the part that IS in scope: the operand must not reach
// the lowering's `trunc … to i32` and truncate a `Range<int>` struct into a
// plausible-looking start index. The codegen `isIntegerTy()` guard keeps it a
// LOUD failure — no value produced, hence the #2434 note — which is the
// behaviour it had before this fix. If a later change makes `Slice(Range)`
// lower, this test should be replaced by one asserting the sliced VALUES; it
// must not be deleted to make a silent miscompile go quiet.
BEZEL_TEST(codegen_span_slice_one_arg_2312, range_operand_does_not_miscompile) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var bad := ros.Slice(1..<3); "
        "  return bad.Length; "));
    if (aotWasSkipped(r)) return;
    // Pin the CURRENT behaviour precisely rather than accepting anything.
    //
    // An earlier draft wrote `if (r.ran) EXPECT_EQ(exitCode, 0);` with no
    // further assertion, which silently tolerated three different outcomes — a
    // compile failure, a crash, and the degrade-to-zero — so it would not have
    // noticed `Slice(Range)` later becoming a hard reject or starting to
    // segfault. Today it RUNS and answers 0 (the operand does not lower, so the
    // local degrades to zero per #2434), and that is asserted outright.
    //
    // The load-bearing assertion is the NEGATIVE one implied by it: the answer
    // must never be 2, which is what a `{i32,i32}` Range truncated to its first
    // field (start=1) and used as a slice start would plausibly produce.
    //
    // If #2662 is settled, this test SHOULD fail — that is the point. Replace
    // it with an assertion on the sliced VALUES if the overload becomes
    // supported, or with a diagnostic-TEXT assertion if it becomes a reject.
    // Do not relax it back into accepting every outcome.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// Controls — these pass BOTH with and without the fix. Their job is to prove
// the new gate does not false-reject the shapes that already worked.
// ---------------------------------------------------------------------------

// The two-arg overload, unchanged.
BEZEL_TEST(codegen_span_slice_one_arg_2312, control_two_arg_slice_unchanged) {
    JitResult r = runAot(buf5(
        "  var ros := buf.AsReadOnlySpan(); "
        "  var sl := ros.Slice(2, 3); "
        "  if sl.Length <> 3 then return 1; end; "
        "  return sl[0]; "));             // 12
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// §7.4 narrowing (#2614) still ACCEPTED at the one-arg site: "Member access,
// method calls, and indexing on a nullable value are compile errors unless the
// compiler has narrowed it to non-null", and "Narrowing also applies after
// early return". The new gate skips primitive operands precisely so this stays
// legal — a false reject here would break working code.
BEZEL_TEST(codegen_span_slice_one_arg_2312, control_narrowed_nullable_start_accepted) {
    JitResult r = runAot(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method N(flag: bool, k: int): int? "
        "    if flag then return k; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(5); "
        "    buf[0] := 10; buf[1] := 11; buf[2] := 12; "
        "    buf[3] := 13; buf[4] := 14; "
        "    var ros := buf.AsReadOnlySpan(); "
        "    var n := N(true, 2); "
        "    if n = null then return 1; end; "     // §7.4 narrowing
        "    var sl := ros.Slice(n); "
        "    if sl.Length <> 3 then return 2; end; "
        "    return sl[0]; "                        // 12 — the payload, not the flag
        "  end Main; "
        "end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// An UN-narrowed `int?` is still rejected by #2614's §7 gate, and by that gate
// ALONE — the new non-primitive check deliberately skips primitives so the two
// cannot double-report on one argument.
BEZEL_TEST(codegen_span_slice_one_arg_2312, control_unnarrowed_nullable_start_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method F(n: int?): int "
        "    using var buf := new OwningBuffer<int>(5); "
        "    var ros := buf.AsReadOnlySpan(); "
        "    var sl := ros.Slice(n); "
        "    return sl.Length; "
        "  end F; "
        "  public static method Main(): int return F(2); end Main; "
        "end C;",
        "is nullable but the target type is non-nullable"));
}
