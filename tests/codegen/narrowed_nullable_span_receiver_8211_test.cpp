// Regression test for issue #8211.
//
// spec §7.4 (v0.355): "Member access, method calls, and indexing on a
// nullable value are compile errors unless the compiler has narrowed it to
// non-null"; §22.6: "a read of a binding §7.4 has narrowed non-null is of its
// non-nullable type"; §7.5: "`x?.M` returns null if `x` is null; otherwise it
// performs the access. The result type is always nullable". stdlib §3.3: the
// `ReadOnlyStringSpan` view (`Source: string`, `Start: int`, `Length: int`)
// and its `Slice(start, length)`; stdlib §4.14/§4.15: the `Span<T>` /
// `ReadOnlySpan<T>` / `ArraySpan<T>` / `ReadOnlyArraySpan<T>` family.
//
// THE DEFECT. The issue's own repro is
//
//   var sp := new ReadOnlyStringSpan(w, 0, 5);
//   return sp?.Slice(1, 3);            // a ReadOnlyStringSpan?  (§7.5)
//
// and the CONSUMER of that nullable span — `s.Length` under `if s <> null`
// — crashed the compiler: bezelc died with SIGSEGV (exit 139) inside
// `llvm::Value::setName`, reached from `Emitter::lowerExprInner`'s span
// member-read arm (LlvmBackend_Exprs.cpp), which does
// `builder_.CreateExtractValue(sv, {2u}, "sp.len")` on the receiver's
// lowered value.
//
// A nullable span is a NULLABLE VALUE type: it lowers to the tagged
// `{i8 hasValue; {ptr,i32,i32}}` carrier (isNullableValueStructBuiltinTy /
// nullableValueStructTy), so the read of a §7.4-narrowed `s` still loads the
// CARRIER, while the span member read wants the PAYLOAD — the {ptr,i32,i32}
// struct whose field 2 is the length. `CreateExtractValue` with index 2 on a
// two-element carrier is out of range: in a release LLVM that is an
// out-of-bounds extract, and the crash is what it produced.
//
// The same defect, one layer down, hit the OTHER span members: `.IsEmpty`
// (the same out-of-range extract) and `.Start` (index 1 — the carrier's
// PAYLOAD, read as the span's own start field), and the method-call
// receiver (`s.Slice(1, 3)`) and the indexer (`s[1]`), which spill the
// receiver into a `{ptr,i32,i32}` slot and so read the carrier's tag byte
// as the source pointer: an IndexOutOfRangeException at run time, and a
// SIGSEGV for `s.ToString()`.
//
// THE FIX (sema, one line of scope, exactly #6842's precedent). §7.4's
// member-access gate ALREADY rejects the un-narrowed read (control row
// below), and §22.6 says the narrowed read IS of the non-nullable type — so
// the gate now PUBLISHES the narrowed read of a span-family receiver through
// `SemaContext::markNarrowedValueRead`, the #6674 mechanism, and codegen's
// ONE central unwrap (`Emitter::lowerExpr`) extracts the payload for every
// consumer (member read, method call, indexer) instead of each lowering site
// repeating the unwrap.
//
// THE TESTS. T1 is the issue's exact repro and pins its STDOUT ("300"), not
// "it did not crash": a crash fix that only avoided the crash would pass a
// weaker assertion. Every row asserts the program's output bytes or a
// diagnostic's exact wording.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// §3.3: a `ReadOnlyStringSpan` over the first five code points of a string.
const char* kStringSpan =
    "  var w := \"hello\"; "
    "  var sp := new ReadOnlyStringSpan(w, 0, 5); ";

// §4.15: the array-backed span twin over an `Array<int>`.
const char* kArraySpan =
    "  using var a := new Array<int>(); "
    "  a.Add(10); a.Add(20); a.Add(30); "
    "  var sp := new ArraySpan<int>(a, 1, 2); ";

}  // namespace

// ===========================================================================
// T1 — THE PIN. The issue's exact repro: `sp?.Slice(1, 3)` over a
// NON-NULLABLE span receiver (§7.5 — the result type is always nullable), and
// the narrowed `s.Length` read of that nullable span. Pre-fix: bezelc died
// with SIGSEGV (exit 139) while compiling this program.
// per §7.5 / §7.4 / §22.6 / stdlib §3.3, v0.355.
//
// The LENIENT checked oracle (ASan on, LeakSanitizer off) is the right one for
// THIS program, and the reason is the ISSUE'S OWN SHAPE, not this fix: the
// span `sp` ESCAPES `F` (it is returned), so the source string it borrows
// keeps the conservative escape and is not released — the "safe direction" the
// stdlib §3.3 fix for #8162 documents ("for those the source keeps the
// conservative escape, so it is NOT released (a leak — the safe direction)").
// MEASURED under `bezelc --sanitize=address`: "3990 byte(s) leaked in 200
// allocation(s)" for the 100-iteration loop, every byte of it the source
// string — #8162's open subject, not this issue's. LeakSanitizer ON would
// therefore fail the row for a reason that has nothing to do with #8211, so
// the row runs the checked oracle with LSan off (ASan — the memory-safety
// coverage — stays on).
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           issue_repro_8211) {
    auto r = runAotCheckedLenient(
        "module Conf; public class Program "
        "  static method F(i: int): ReadOnlyStringSpan? "
        "    var w := \"hello\" + i.ToString(); "
        "    var sp := new ReadOnlyStringSpan(w, 0, 5); "
        "    return sp?.Slice(1, 3); "
        "  end F; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0 .. 99 do "
        "      var s := F(i); "
        "      if s <> null then t := t + s.Length; end if; "
        "    end for; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    // 100 iterations (0..99 is INCLUSIVE, §16.5) x the sliced length 3.
    BEZEL_EXPECT_EQ(r.stdoutText, "300\n");
}

// ===========================================================================
// T2 — the narrowed nullable span's `.Length` (the crash site itself).
// per §3.3 / §7.4 / §22.6, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_length_read) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  var s: ReadOnlyStringSpan? := sp; "
        "  if s <> null then Console.WriteLine(s.Length); end if; "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// ===========================================================================
// T3 — the sibling span members on the same narrowed receiver: `.Start`
// (field 1) and `.IsEmpty` (field 2 compared with 0). Pre-fix: `.Start`
// emitted MALFORMED IR (the carrier's payload read as the start field) and
// `.IsEmpty` crashed the compiler.
// per stdlib §3.3 / §4.14 / §7.4, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_start_and_isempty) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  var s: ReadOnlyStringSpan? := sp; "
        "  if s <> null then "
        "    Console.WriteLine(s.Start); "
        "    Console.WriteLine(s.IsEmpty); "
        "  end if; "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\nFalse\n");
}

// ===========================================================================
// T4 — `.Source` (field 0, the source string) on the narrowed receiver.
// per stdlib §3.3 / §7.4, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_source_read) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  var s: ReadOnlyStringSpan? := sp; "
        "  if s <> null then Console.WriteLine(s.Source); end if; "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "hello\n");
}

// ===========================================================================
// T5 — the METHOD-CALL receiver on the narrowed nullable span: `s.Slice(1, 3)`
// must slice the span, not read the carrier's tag byte as the source
// pointer (pre-fix: an IndexOutOfRangeException at run time).
// per stdlib §3.3 / §7.4 / §22.6, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_method_call_receiver) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  var s: ReadOnlyStringSpan? := sp; "
        "  var n := 0; "
        "  if s <> null then n := s.Slice(1, 3).Length; end if; "
        "  Console.WriteLine(n); "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ===========================================================================
// T6 — the ARRAY-backed span twin (`ArraySpan<T>`, stdlib §4.15) on the same
// narrowed nullable receiver: the whole span family shares the
// {ptr,i32,i32} shape, so the whole family shares the defect.
// per stdlib §4.15 / §7.4, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_array_span_length_read) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kArraySpan +
        "  var s: ArraySpan<int>? := sp; "
        "  if s <> null then Console.WriteLine(s.Length); end if; "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// ===========================================================================
// T7 — the INDEXER on the narrowed nullable span: `s[1]` reads the code
// point at the span's own start + 1 (stdlib §3.3's indexer). Pre-fix: the
// carrier's tag byte was read as the source pointer, and the program raised
// IndexOutOfRangeException on a valid index.
// per stdlib §3.3 / §7.4, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           narrowed_indexer_read) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  var s: ReadOnlyStringSpan? := sp; "
        "  var n := 0; "
        "  if s <> null then n := int(s[1]); end if; "
        "  Console.WriteLine(n); "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "101\n");  // 'e'
}

// ===========================================================================
// T8 — CONTROL: the UN-narrowed read is still the §7.4 compile error (the
// gate this fix relies on: the payload unwrap is sound only because sema
// rejects the un-narrowed form). The diagnostic TEXT is pinned.
// per §7.4, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           unnarrowed_receiver_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x: ReadOnlyStringSpan? := null; "
        "  Console.WriteLine(x.Length); "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(r) &&
        bezel::test::diagnosticMessageContains(
            r, "on a nullable value requires narrowing it to non-null first (§7.4)"));
}

// ===========================================================================
// T9 — CONTROL: the NON-nullable span is untouched (the ordinary `.` read
// still reads the payload, and the `?.` spelling over it still yields the
// §7.5 nullable result). The fix must not change this arm.
// per §7.5 / stdlib §3.3, v0.355.
// ===========================================================================
BEZEL_TEST(codegen_narrowed_nullable_span_receiver_8211,
           non_nullable_span_unchanged) {
    auto r = runAotChecked(
        std::string("module M; class C public static method Main(): int ") +
        kStringSpan +
        "  Console.WriteLine(sp.Length); "
        "  var q := sp?.Slice(1, 3); "
        "  Console.WriteLine(q = null); "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\nFalse\n");
}
