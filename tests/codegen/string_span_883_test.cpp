// Codegen + sema — #883: string.AsSpan() and the ReadOnlyStringSpan view type
// (stdlib §9.6 / §3.3).
//
// Root cause (pre-fix): `ReadOnlyStringSpan` was not registered in
// builtinTypeArity() ("unresolved type name") and `string.AsSpan()` had no
// intrinsic branch ("string member .AsSpan is not yet supported in codegen").
//
// Fix: register ReadOnlyStringSpan (a non-generic System.Text record viewing a
// contiguous code-point range of a string: Source: string, Start: int,
// Length: int — §3.3) and lower `string.AsSpan()` (+ the (start), (start,length),
// (Range) overloads) to a {string-ptr, i32, i32} span struct (the bz.arrayspan
// shape; field 0 is a string pointer). The span's indexer reads the (Start + i)
// CODE POINT via loadCodePoint (NOT array element access); Length/IsEmpty are
// field reads; Slice reconstructs a sub-view.
//
// The substring-search methods (IndexOf/Contains/StartsWith/EndsWith/
// SequenceEqual) are deferred (search over the adaptive 1/2/4-byte string
// encoding) — they are a clean sema reject pointing at the follow-up issue, not
// an accept-trap.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejectedAtCompile(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
}
// Rejected AND the diagnostic says `needle`. Used where the pre-fix compiler
// also rejected, but for a DIFFERENT reason — the text is what discriminates.
bool rejectedWith(const std::string& prog, const char* needle) {
    auto r = runAot(prog);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// §9.6 / §3.3 (#883): the issue's exact repro — AsSpan().Length over "hello".
BEZEL_TEST(string_span_883, asspan_length_repro) {
    BEZEL_EXPECT_EQ(run(
        "module Repro2; class C public static method Main(): int "
        "  var s: string := \"hello\"; "
        "  var span: ReadOnlyStringSpan := s.AsSpan(); "
        "  return span.Length; "  // 5
        "end Main; end C;"), 5);
}

// §3.3: the span indexer reads code points (chars) — sp[0]='A'(65), sp[4]='E'(69).
// #5054 (§3.5, v0.307) closed `char`'s arithmetic-operator exclusion, so a
// `char + char` (or `int + char`) sum now needs the explicit `int(c)`
// conversion the diagnostic itself names; the span-indexer VALUE under test
// is unchanged.
BEZEL_TEST(string_span_883, indexer_reads_code_points) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABCDE\"; "
        "  var sp := s.AsSpan(); "
        "  return int(sp[0]) + int(sp[4]); "  // 65 + 69 == 134
        "end Main; end C;"), 134);
}

// §3.3: AsSpan(start) — view from an offset to the end (Start is applied to the
// indexer, so sp[0] reads the 'l' at source index 2).
// #5054 (§3.5, v0.307): sp[0] is `char`; `int(...)` per the diagnostic's remedy.
BEZEL_TEST(string_span_883, asspan_start_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(2); "  // "llo"
        "  return sp.Length * 10 + int(sp[0]); "  // 3*10 + 'l'(108) == 138
        "end Main; end C;"), 138);
}

// §3.3: AsSpan(start, length) — a bounded view; "world" from "hello world".
// #5054 (§3.5, v0.307): sp[0] is `char`; `int(...)` per the diagnostic's remedy.
BEZEL_TEST(string_span_883, asspan_start_length_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  var sp := s.AsSpan(6, 5); "  // "world"
        "  return sp.Length * 10 + int(sp[0]); "  // 5*10 + 'w'(119) == 169
        "end Main; end C;"), 169);
}

// §3.3: Slice(start, length) returns a ReadOnlyStringSpan over the same source.
// #5054 (§3.5, v0.307): sl[0] is `char`; `int(...)` per the diagnostic's remedy.
BEZEL_TEST(string_span_883, slice_subview) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(); "
        "  var sl := sp.Slice(1, 3); "  // "ell"
        "  return sl.Length * 10 + int(sl[0]); "  // 3*10 + 'e'(101) == 131
        "end Main; end C;"), 131);
}

// §3.3: IsEmpty is false for a non-empty span.
BEZEL_TEST(string_span_883, isempty_false_for_nonempty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  var sp := s.AsSpan(); "
        "  if sp.IsEmpty then return 1; end; "
        "  return 7; "
        "end Main; end C;"), 7);
}

// §3.3: ReadOnlyStringSpan is usable as a PARAMETER type (registered builtin).
BEZEL_TEST(string_span_883, readonlystringspan_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Len(span: ReadOnlyStringSpan): int "
        "    return span.Length; "
        "  end Len; "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    return Len(s.AsSpan()); "  // 5
        "  end Main; end C;"), 5);
}

// §3.3 / §31.2: an out-of-range index raises IndexOutOfRangeException (bounds are
// the span's Length, not the source string's length). Caught here so the test
// asserts the throw without an uncaught exception escaping the JIT.
BEZEL_TEST(string_span_883, index_out_of_range_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\"; "
        "  var sp := s.AsSpan(); "
        "  try "
        "    var c := sp[5]; "
        "    return c; "
        "  catch IndexOutOfRangeException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// §3.3 / #971 item 1: the substring-search family now RESOLVES instead of being
// deferred. (This case replaces #883's `search_methods_deferred_clean_reject`,
// which asserted the opposite — that `sp.IndexOf(os)` was a clean sema reject.
// That assertion was the recorded deferral of #971 item 1, so implementing the
// item necessarily retires it; the shape it guarded — never an accept-trap — is
// still covered, by the operand/arity rejects at the bottom of this file.)
// "hello".AsSpan().IndexOf("ell".AsSpan()) == 1.
BEZEL_TEST(string_span_883, indexof_span_operand_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(); "
        "  var other := \"ell\"; "
        "  var os := other.AsSpan(); "
        "  return sp.IndexOf(os); "
        "end Main; end C;"), 1);
}

// §3.3 / §29.2 / #971 item 2: `for var c in sp` over a ReadOnlyStringSpan
// iterates the span's code points in order (source-relative offset
// start..start+len). Sum-of-codes for "hello" is 104+101+108+108+111 = 532.
// Uses runAotOut (write the sum to stdout) because 532 exceeds the 8-bit
// exit-code channel that runAot reads.
// fix #5125 (§3.5, v0.307): respelled `sum + c` as `sum + int(c)` — the
// span for-in loop variable is `char` and char arithmetic is rejected;
// these rows previously relied on a sema gap (the loop variable's type
// never reached the §3.5 char-arithmetic gate).
BEZEL_TEST(string_span_883, for_in_iterates_full_span_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(); "
        "  var sum := 0; "
        "  for var c in sp do sum := sum + int(c); end; "
        "  Console.WriteLine(sum); "
        "  return 0; "
        "end Main; end C;"), "532");
}

// §3.3 / §29.2 / #971 item 2: for-in over a SLICED span iterates ONLY the
// sliced sub-range. "hello world".AsSpan(6, 5) -> "world"; sum = 119+111+
// 114+108+100 = 552. Uses runAotOut (552 > 255).
BEZEL_TEST(string_span_883, for_in_iterates_sliced_span_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  var sp := s.AsSpan(6, 5); "
        "  var sum := 0; "
        "  for var c in sp do sum := sum + int(c); end; "
        "  Console.WriteLine(sum); "
        "  return 0; "
        "end Main; end C;"), "552");
}

// §3.3 / §29.2 / #971 item 2: for-in over a span produced by
// `s.AsSpan(start, length)` (the `(start, length)` overload) iterates the
// sub-range. "hello".AsSpan(1, 3) -> "ell"; sum = 101+108+108 = 317. Uses
// runAotOut (317 > 255). (.Slice() is not yet wired in codegen; this is the
// shape that DOES go through the span's start/length fields.)
BEZEL_TEST(string_span_883, for_in_iterates_slice_subview_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(1, 3); "
        "  var sum := 0; "
        "  for var c in sp do sum := sum + int(c); end; "
        "  Console.WriteLine(sum); "
        "  return 0; "
        "end Main; end C;"), "317");
}

// §3.3 / §29.2 / #971 item 2: for-in over a ZERO-LENGTH span is a no-op
// (the body never runs; sum stays 0). Verifies the condBB's i32-widened
// length-0 comparison works without an OOB read on the empty sub-range.
BEZEL_TEST(string_span_883, for_in_over_zero_length_span_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(2, 0); "
        "  var sum := 0; "
        "  for var c in sp do sum := sum + int(c); end; "
        "  return sum; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// #971 item 1 — the §3.3 search / compare family.
//
// Every case below was a hard sema REJECT before the fix ("`IndexOf` on
// `ReadOnlyStringSpan` is not yet supported…"), so `run(...)` returned the
// -999 compile-failure sentinel and each value assertion failed. They pass only
// once the family is typed in sema and lowered to the bz_strspan_* runtime
// helpers.
// ===========================================================================

// §3.3 "Searches": IndexOf(value: char) walks the span's range and returns the
// first index whose code point matches, or -1. "hello".AsSpan(): 'l' is at 2.
BEZEL_TEST(string_span_883, indexof_char_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  return s.AsSpan().IndexOf('l'); "
        "end Main; end C;"), 2);
}

// §3.3 "Searches": IndexOf(value: string) — the convenience overload,
// "equivalent to calling the ReadOnlyStringSpan overload after value.AsSpan()".
// "hello world".AsSpan().IndexOf("wor") == 6.
BEZEL_TEST(string_span_883, indexof_string_operand_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  return s.AsSpan().IndexOf(\"wor\"); "
        "end Main; end C;"), 6);
}

// §3.3: the returned index is "the start index of the match in the RECEIVER" —
// span-relative, NOT an index into the source string. This is the case a
// naive delegation to the whole-string bz_str_indexof would get wrong:
// "xxhello world".AsSpan(2) is "hello world", so "wor" is at 6, not 8.
BEZEL_TEST(string_span_883, indexof_is_relative_to_span_start_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"xxhello world\"; "
        "  return s.AsSpan(2).IndexOf(\"wor\"); "
        "end Main; end C;"), 6);
}

// §3.3: the search is BOUNDED BY THE SPAN — an occurrence that lies outside the
// viewed range is not a match. "abcdef".AsSpan(0, 3) is "abc"; "def" is present
// in the SOURCE but not in the span, so IndexOf returns -1 (100 + -1 == 99,
// keeping the value inside the 8-bit exit channel).
BEZEL_TEST(string_span_883, indexof_bounded_by_span_end_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abcdef\"; "
        "  return 100 + s.AsSpan(0, 3).IndexOf(\"def\"); "
        "end Main; end C;"), 99);
}

// §3.3: a needle that would OVERRUN the span's end is not a match either —
// "abcdef".AsSpan(0, 4) is "abcd", and the "cde" starting at span index 2 runs
// one code point past the span, so IndexOf returns -1.
BEZEL_TEST(string_span_883, indexof_needle_overrunning_span_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abcdef\"; "
        "  return 100 + s.AsSpan(0, 4).IndexOf(\"cde\"); "
        "end Main; end C;"), 99);
}

// §3.3 "Searches": Contains(value) is defined as IndexOf(value) >= 0, in both
// the char and the substring form. Here: Contains("wor") true (+1),
// Contains('z') false, Contains("zzz") false.
BEZEL_TEST(string_span_883, contains_char_and_substring_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sp := \"hello world\".AsSpan(); "
        "  var r := 0; "
        "  if sp.Contains(\"wor\") then r := r + 1; end; "
        "  if sp.Contains('e') then r := r + 10; end; "
        "  if sp.Contains('z') then r := r + 100; end; "
        "  if sp.Contains(\"zzz\") then r := r + 1000; end; "
        "  return r; "
        "end Main; end C;"), 11);
}

// §3.3 "Prefix and suffix": StartsWith/EndsWith compare code-point-by-code-point
// and are anchored at the SPAN's ends, not the source's. "hello world".AsSpan(
// 0, 5) is "hello": EndsWith("llo") holds, EndsWith("rld") does not even though
// the SOURCE ends with "rld".
BEZEL_TEST(string_span_883, startswith_endswith_anchored_to_span_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sp := \"hello world\".AsSpan(0, 5); "
        "  var r := 0; "
        "  if sp.StartsWith(\"hell\") then r := r + 1; end; "
        "  if sp.EndsWith(\"llo\") then r := r + 10; end; "
        "  if sp.EndsWith(\"rld\") then r := r + 100; end; "
        "  return r; "
        "end Main; end C;"), 11);
}

// §3.3 "Equality": SequenceEqual is true iff the two spans have the SAME LENGTH
// and matching code points at every position — a prefix is not equal. The full
// "hello" span is not SequenceEqual to "hell"; the AsSpan(0, 4) subview is.
BEZEL_TEST(string_span_883, sequenceequal_is_length_sensitive_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var r := 0; "
        "  if s.AsSpan().SequenceEqual(\"hell\") then r := r + 1; end; "
        "  if s.AsSpan(0, 4).SequenceEqual(\"hell\") then r := r + 10; end; "
        "  if s.AsSpan().SequenceEqual(s.AsSpan()) then r := r + 100; end; "
        "  return r; "
        "end Main; end C;"), 110);
}

// §3.3 "Why a separate type from ReadOnlySpan<char>" / language spec §4.6: a
// string's storage is adaptive (1/2/4 bytes per code point) and the two operands
// of a search may have DIFFERENT widths. "aŁb" holds U+0141, so it is stored at
// width 2, while "a" and "ab" are width 1. Comparison is by code-point VALUE, so
// every one of these must be decided correctly across the width boundary — this
// is exactly the case a byte-level (memcmp/memmem) implementation would fail.
// Expected: Contains("Ł") 1, StartsWith("a") 10, narrow.Contains("Ł") false,
// AsSpan(1,1).SequenceEqual("Ł") 1000 => 1011.
BEZEL_TEST(string_span_883, search_across_mixed_storage_widths_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var wide := \"a\xC5\x81""b\"; "
        "  var narrow := \"ab\"; "
        "  var r := 0; "
        "  if wide.AsSpan().Contains(\"\xC5\x81\") then r := r + 1; end; "
        "  if wide.AsSpan().StartsWith(\"a\") then r := r + 10; end; "
        "  if narrow.AsSpan().Contains(\"\xC5\x81\") then r := r + 100; end; "
        "  if wide.AsSpan(1, 1).SequenceEqual(\"\xC5\x81\") then r := r + 1000; end; "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;"), "1011");
}

// §3.3: a ZERO-LENGTH span finds nothing and — critically — reads nothing. Every
// member of the family is exercised on an empty view positioned at the very end
// of the source, so any off-by-one would read past the string's data buffer
// (the case generated-code ASan would catch). All searches must report
// not-found: 100 + (-1) + (-1) == 98, with every bool arm false.
BEZEL_TEST(string_span_883, search_on_zero_length_span_reads_nothing_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var z := \"hello\".AsSpan(5, 0); "
        "  var r := 100; "
        "  r := r + z.IndexOf(\"h\"); "
        "  r := r + z.IndexOf('h'); "
        "  if z.Contains(\"h\") then r := r + 1000; end; "
        "  if z.StartsWith(\"h\") then r := r + 2000; end; "
        "  if z.EndsWith(\"h\") then r := r + 4000; end; "
        "  if z.SequenceEqual(\"h\") then r := r + 8000; end; "
        "  return r; "
        "end Main; end C;"), 98);
}

// §3.3 / #971 item 1: the family works on a span reaching a method through a
// `ReadOnlyStringSpan` PARAMETER — the shape §3.3 exists for (a tokenizer/parser
// helper taking a view). "content-type: text".IndexOf("type") == 8.
BEZEL_TEST(string_span_883, search_through_span_parameter_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private static method Find(hay: ReadOnlyStringSpan, needle: string): int "
        "    return hay.IndexOf(needle); "
        "  end Find; "
        "  public static method Main(): int "
        "    var s := \"content-type: text\"; "
        "    return Find(s.AsSpan(), \"type\"); "
        "  end Main; end C;"), 8);
}

// §3.3 (v0.195) — `Slice(start)`, the one-arg form (`Slice(start, Length -
// start)`). Before the fix this was an ACCEPT-TRAP, not a reject: sema typed it
// as a ReadOnlyStringSpan but codegen's branch required exactly two arguments,
// fell through to `return nullptr`, and left the result local holding an
// undefined value (the probe returned a different garbage exit code per run).
// "hello world".AsSpan().Slice(6) is "world": Length 5, SequenceEqual holds, and
// IndexOf("rld") == 2 within the sliced view => 5*10 + 1 + 2*100 == 251.
BEZEL_TEST(string_span_883, slice_start_only_overload_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var sp := \"hello world\".AsSpan().Slice(6); "
        "  var r := sp.Length * 10; "
        "  if sp.SequenceEqual(\"world\") then r := r + 1; end; "
        "  r := r + sp.IndexOf(\"rld\") * 100; "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;"), "251");
}

// §3.3 / §31.2: the one-arg `Slice(start)` keeps the family's bounds contract —
// a start past the span's Length raises IndexOutOfRangeException rather than
// producing an out-of-range view. (Before the fix it produced garbage silently.)
BEZEL_TEST(string_span_883, slice_start_only_out_of_range_throws_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var sp := \"hi\".AsSpan(); "
        "  try "
        "    var sl := sp.Slice(5); "
        "    return sl.Length; "
        "  catch IndexOutOfRangeException => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 99);
}

// §3.3 "Materialization": ToString() returns a string holding the span's code
// points. Before the fix this SEGFAULTED: the universal `Object.ToString`
// typing rule (§8.5, #608) matched the span's Named/no-TypeSymbol TypeRef, so
// codegen dispatched through vtable slot 3 — but a span is a {ptr, i32, i32}
// struct VALUE with no vtable, so the load read the source string's first eight
// bytes as a vtable pointer and jumped through it.
BEZEL_TEST(string_span_883, tostring_materializes_subview_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"hello world\".AsSpan(6, 5).ToString()); "
        "  return 0; "
        "end Main; end C;"), "world");
}

// §3.3 "Materialization": the materialized string is an ordinary `string` —
// equality against a literal holds, and it survives a wide (width-2) source, so
// the code points are not truncated on the way out. "xŁłz".AsSpan(1,2)
// materializes to "Łł".
BEZEL_TEST(string_span_883, tostring_preserves_wide_code_points_971) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t := \"x\xC5\x81\xC5\x82z\".AsSpan(1, 2).ToString(); "
        "  if t = \"\xC5\x81\xC5\x82\" then return t.Length + 7; end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// §3.3 / §28.9: ToString() mints a fresh string at rc := 1, so it must be
// recognized as a +1 producer (isPlusOneStringProducer) — otherwise a consumer
// retains on top of the producer's reference and the string leaks every
// iteration (the #2199 shape). 200 materializations of a 5-code-point span sum
// to 1000; under the `--full` ASan/LSan gate a missing registration surfaces
// here as a leak report rather than a wrong value.
BEZEL_TEST(string_span_883, tostring_is_plus_one_producer_no_leak_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  var i := 0; var n := 0; "
        "  while i < 200 do "
        "    var t := s.AsSpan(6, 5).ToString(); "
        "    n := n + t.Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;"), "1000");
}

// §3.3 / §9.7 (v0.195): a ReadOnlyStringSpan is an ORDINARY record, not a
// borrow-bearing one — its only reference-typed field is the runtime-managed
// `Source` string — so it may sit in a class field and outlive the enclosing
// scope. The search family must work on such a field-held span too (guards the
// #1251 field-span path against a regression from this change). The field views
// "world": 5*10 + SequenceEqual + IndexOf("rld")*100 == 251.
BEZEL_TEST(string_span_883, search_on_field_held_span_971) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Holder "
        "  public Sp: ReadOnlyStringSpan; "
        "  public constructor(s: string) this.Sp := s.AsSpan(6, 5); end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(\"hello world\"); "
        "  var r := h.Sp.Length * 10; "
        "  if h.Sp.SequenceEqual(\"world\") then r := r + 1; end; "
        "  r := r + h.Sp.IndexOf(\"rld\") * 100; "
        "  Console.WriteLine(r); "
        "  return 0; "
        "end Main; end C;"), "251");
}

// §3.3: the surface stays exactly what the document declares — StartsWith has a
// ReadOnlyStringSpan and a string overload and NO char overload, so a char
// argument is rejected. The pre-fix compiler also rejected this line (with the
// blanket "not yet supported" deferral), so the DIAGNOSTIC TEXT is what
// discriminates: the reject must now name the declared operand set.
BEZEL_TEST(string_span_883, startswith_has_no_char_overload_971) {
    BEZEL_EXPECT_EQ(rejectedWith(
        "module M; class C public static method Main(): int "
        "  if \"hello\".AsSpan().StartsWith('h') then return 1; end; "
        "  return 0; "
        "end Main; end C;",
        "takes a `ReadOnlyStringSpan` or a `string`"), true);
}

// §3.3: an operand of a type the document declares for NO overload (here `int`)
// is rejected, and the diagnostic names the three declared IndexOf operands.
// Same discrimination note as above — the text, not the rejection, is the assert.
BEZEL_TEST(string_span_883, indexof_rejects_undeclared_operand_971) {
    BEZEL_EXPECT_EQ(rejectedWith(
        "module M; class C public static method Main(): int "
        "  return \"hello\".AsSpan().IndexOf(3); "
        "end Main; end C;",
        "takes a `char`, a `ReadOnlyStringSpan`, or a `string`"), true);
}

// §3.3: `Slice` takes `(start)` or `(start, length)` — nothing else. Before the
// fix a three-argument call was an ACCEPT-TRAP: sema typed it as a span and
// codegen's two-argument guard let it fall through to an undefined value, so it
// compiled and ran to garbage. It is now a compile-time reject.
BEZEL_TEST(string_span_883, slice_wrong_arity_rejected_971) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  var sl := \"hello\".AsSpan().Slice(1, 2, 3); "
        "  return sl.Length; "
        "end Main; end C;"), true);
}

// §3.3 / §9.6 / §7 (#2581): an un-narrowed `string?` operand to a
// ReadOnlyStringSpan search/compare method is now REJECTED at compile time —
// the same §7 nullable->non-nullable rule a user `method Take(x: string)`
// enforces, and the same rule #2315 closed for the primitive `string` instance
// family. Previously this was an ACCEPT-TRAP (the builtin span path had no
// MethodSymbol, so the per-argument assignability check never ran): the operand
// reached codegen and the runtime helper's accidental null guard converted the
// type error into a "not found"/wrong answer (100 + (-1) == 99). An earlier
// draft of this test pinned that buggy runtime value (99); #2581 closes the
// hole in sema, so the span family now MATCHES its sibling by rejecting too.
BEZEL_TEST(string_span_883, null_string_operand_matches_string_family_971) {
    BEZEL_EXPECT_EQ(rejectedWith(
        "module M; class C public static method Main(): int "
        "  var n: string? := null; "
        "  var sp := \"hello\".AsSpan(); "
        "  var r := 100 + sp.IndexOf(n); "
        "  return r; "
        "end Main; end C;",
        "is nullable but the target type is non-nullable"), true);
}

// §3.3 (v0.109): `CopyTo(destination: Span<char>)` is DECLARED but not yet
// lowered — the span-destination write family, distinct from the read/search
// surface implemented for #971 item 1. It must stay a clean reject naming what
// is missing (never an accept-trap), and the "documented surface" list in the
// unknown-member diagnostic must no longer claim the search family is absent.
BEZEL_TEST(string_span_883, copyto_still_deferred_clean_reject_971) {
    BEZEL_EXPECT_EQ(rejectedWith(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwningBuffer<char>(5); "
        "  \"hello\".AsSpan().CopyTo(buf.AsSpan()); "
        "  return 0; "
        "end Main; end C;",
        "`CopyTo` on `ReadOnlyStringSpan` is not yet supported"), true);
}
