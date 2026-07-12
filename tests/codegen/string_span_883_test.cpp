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

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
bool rejectedAtCompile(const std::string& prog) {
    auto r = runAot(prog);
    return !r.ran && r.diagnostics.hasErrors();
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
BEZEL_TEST(string_span_883, indexer_reads_code_points) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABCDE\"; "
        "  var sp := s.AsSpan(); "
        "  return sp[0] + sp[4]; "  // 65 + 69 == 134
        "end Main; end C;"), 134);
}

// §3.3: AsSpan(start) — view from an offset to the end (Start is applied to the
// indexer, so sp[0] reads the 'l' at source index 2).
BEZEL_TEST(string_span_883, asspan_start_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(2); "  // "llo"
        "  return sp.Length * 10 + sp[0]; "  // 3*10 + 'l'(108) == 138
        "end Main; end C;"), 138);
}

// §3.3: AsSpan(start, length) — a bounded view; "world" from "hello world".
BEZEL_TEST(string_span_883, asspan_start_length_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  var sp := s.AsSpan(6, 5); "  // "world"
        "  return sp.Length * 10 + sp[0]; "  // 5*10 + 'w'(119) == 169
        "end Main; end C;"), 169);
}

// §3.3: Slice(start, length) returns a ReadOnlyStringSpan over the same source.
BEZEL_TEST(string_span_883, slice_subview) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(); "
        "  var sl := sp.Slice(1, 3); "  // "ell"
        "  return sl.Length * 10 + sl[0]; "  // 3*10 + 'e'(101) == 131
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

// §3.3 (#883): the substring-search methods are deferred — a clean sema reject
// (not an accept-trap). IndexOf over the adaptive string encoding is the
// follow-up issue.
BEZEL_TEST(string_span_883, search_methods_deferred_clean_reject) {
    BEZEL_EXPECT_EQ(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  var sp := s.AsSpan(); "
        "  var other := \"ell\"; "
        "  var os := other.AsSpan(); "
        "  return sp.IndexOf(os); "
        "end Main; end C;"), true);
}
