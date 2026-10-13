// Codegen + sema — #970: IReadOnlySpan<T> interface for span types
//
// §4.16: IReadOnlySpan<T> is the read-only span interface. Span value types
// (ArraySpan<T>, ReadOnlyArraySpan<T>, Span<T>, ReadOnlySpan<T>) are implicitly
// coercible to IReadOnlySpan<T> for call-bounded parameter passing (§8.6 boxing).
// The synthesized interface exposes:
//   methods:  IndexOf(item: T): int, Contains(item: T): bool,
//             SequenceEqual(other: IReadOnlySpan<T>): bool
//   properties: Length: int, IsEmpty: bool, [i: int]: T
//
// Spec reference: §4.16, §8.6 (v0.156+).
//
// NOTE: new Array<int>(n) sets capacity=n but count=0. All tests that need
// populated arrays use Add() rather than indexer assignment (which would
// bounds-check against count and throw IndexOutOfRangeException at init time).

#include "support/aot_support.hpp"   // runAot — AOT oracle (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include <string>

using bezel::test::runAot;
using bezel::test::JitResult;

#include "test_harness.hpp"

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool rejected(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// §4.16 / §8.6 (#970): core repro — pass ReadOnlyArraySpan<int> where
// IReadOnlySpan<int> is expected; method reads Length + indexer.
BEZEL_TEST(ireadonlyspan_970, repro_sum_via_ireadonly_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Sum(span: IReadOnlySpan<int>): int "
        "    var s := 0; "
        "    var i := 0; "
        "    while i < span.Length do "
        "      s := s + span[i]; "
        "      i := i + 1; "
        "    end; "
        "    return s; "
        "  end; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(3); "
        "    arr.Add(1); arr.Add(2); arr.Add(4); "
        "    return C.Sum(arr.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        7);
}

// §4.16 (#970): Length property through IReadOnlySpan interface.
BEZEL_TEST(ireadonlyspan_970, length_property_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Len(sp: IReadOnlySpan<int>): int "
        "    return sp.Length; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(5); "
        "    a.Add(0); a.Add(0); a.Add(0); a.Add(0); a.Add(0); "
        "    return C.Len(a.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        5);
}

// §4.16 (#970): IsEmpty property — empty span returns true.
BEZEL_TEST(ireadonlyspan_970, is_empty_true_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Check(sp: IReadOnlySpan<int>): int "
        "    if sp.IsEmpty then return 1; else return 0; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(0); "
        "    return C.Check(a.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        1);
}

// §4.16 (#970): IsEmpty property — non-empty span returns false.
BEZEL_TEST(ireadonlyspan_970, is_empty_false_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Check(sp: IReadOnlySpan<int>): int "
        "    if sp.IsEmpty then return 0; else return 1; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3); "
        "    a.Add(99); "
        "    return C.Check(a.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        1);
}

// §4.16 (#970): IndexOf through IReadOnlySpan interface — found case.
BEZEL_TEST(ireadonlyspan_970, indexof_found_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Find(sp: IReadOnlySpan<int>, v: int): int "
        "    return sp.IndexOf(v); "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(4); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "    return C.Find(a.AsReadOnlySpan(), 30); "
        "  end; "
        "end C;"),
        2);
}

// §4.16 (#970): IndexOf — not-found returns -1.
BEZEL_TEST(ireadonlyspan_970, indexof_notfound_spec_4_16) {
    // Value printed, not returned: the exit status is 8-bit and -1 reads as 255.
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Find(sp: IReadOnlySpan<int>, v: int): int "
        "    return sp.IndexOf(v); "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    Console.WriteLine(C.Find(a.AsReadOnlySpan(), 99)); "
        "    return 0; "
        "  end; "
        "end C;");
    // not found → IndexOf returns -1
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// §4.16 (#970): Contains through IReadOnlySpan interface — found.
BEZEL_TEST(ireadonlyspan_970, contains_found_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Has(sp: IReadOnlySpan<int>, v: int): int "
        "    if sp.Contains(v) then return 1; else return 0; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return C.Has(a.AsReadOnlySpan(), 2); "
        "  end; "
        "end C;"),
        1);
}

// §4.16 (#970): Contains — not-found returns false.
BEZEL_TEST(ireadonlyspan_970, contains_notfound_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Has(sp: IReadOnlySpan<int>, v: int): int "
        "    if sp.Contains(v) then return 1; else return 0; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    return C.Has(a.AsReadOnlySpan(), 99); "
        "  end; "
        "end C;"),
        0);
}

// §4.16 (#970): SequenceEqual through IReadOnlySpan interface — equal spans.
BEZEL_TEST(ireadonlyspan_970, seqequal_equal_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Eq(a: IReadOnlySpan<int>, b: IReadOnlySpan<int>): int "
        "    if a.SequenceEqual(b) then return 1; else return 0; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a1 := new Array<int>(3); "
        "    a1.Add(1); a1.Add(2); a1.Add(3); "
        "    using var a2 := new Array<int>(3); "
        "    a2.Add(1); a2.Add(2); a2.Add(3); "
        "    return C.Eq(a1.AsReadOnlySpan(), a2.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        1);
}

// §4.16 (#970): SequenceEqual — different lengths returns false.
BEZEL_TEST(ireadonlyspan_970, seqequal_difflen_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Eq(a: IReadOnlySpan<int>, b: IReadOnlySpan<int>): int "
        "    if a.SequenceEqual(b) then return 1; else return 0; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var a1 := new Array<int>(3); "
        "    a1.Add(1); a1.Add(2); a1.Add(3); "
        "    using var a2 := new Array<int>(2); "
        "    a2.Add(1); a2.Add(2); "
        "    return C.Eq(a1.AsReadOnlySpan(), a2.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        0);
}

// §4.16 (#970): all four span types can box into IReadOnlySpan<int>.
// ReadOnlyArraySpan is tested above; test ArraySpan here.
BEZEL_TEST(ireadonlyspan_970, arrayspan_boxes_into_ireadonly_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Sum(sp: IReadOnlySpan<int>): int "
        "    var s := 0; "
        "    var i := 0; "
        "    while i < sp.Length do s := s + sp[i]; i := i + 1; end; "
        "    return s; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(3); "
        "    a.Add(5); a.Add(3); a.Add(2); "
        "    var sp := a.AsSpan(); "
        "    return C.Sum(sp); "
        "  end; "
        "end C;"),
        10);
}

// §4.16 (#970): IReadOnlySpan<T> usable as a local variable type annotation.
BEZEL_TEST(ireadonlyspan_970, local_annotation_ireadonly_spec_4_16) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a := new Array<int>(2); "
        "    a.Add(7); a.Add(3); "
        "    var sp: IReadOnlySpan<int> := a.AsReadOnlySpan(); "
        "    return sp[0] + sp[1]; "
        "  end; "
        "end C;"),
        10);
}

// §29.2 / §4.16 (#1262): `for var x in sp` over an IReadOnlySpan<T> parameter
// desugars to a direct indexed loop (the box's arrayspan is unboxed and iterated
// like a concrete span). 10+20+30+40 = 100.
BEZEL_TEST(ireadonlyspan_970, for_in_over_ireadonly_span_1262) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Sum(sp: IReadOnlySpan<int>): int "
        "    var t := 0; "
        "    for var x in sp do t := t + x; end; "
        "    return t; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(4); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "    return C.Sum(a.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        100);
}

// §29.2 / §4.16 (#1262): for-in over IReadOnlySpan<T> honours `break`.
BEZEL_TEST(ireadonlyspan_970, for_in_over_ireadonly_span_break_1262) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method First2(sp: IReadOnlySpan<int>): int "
        "    var t := 0; var n := 0; "
        "    for var x in sp do "
        "      if n >= 2 then break; end; "
        "      t := t + x; n := n + 1; "
        "    end; "
        "    return t; "
        "  end; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(4); "
        "    a.Add(10); a.Add(20); a.Add(30); a.Add(40); "
        "    return C.First2(a.AsReadOnlySpan()); "
        "  end; "
        "end C;"),
        30);
}

}  // namespace
