// Codegen + sema — #8135: `ReadOnlyStringSpan.Source` read fails to lower
// (stdlib §3.3).
//
// THE BUG. stdlib §3.3 declares
//
//     public record ReadOnlyStringSpan(Source: string, Start: int, Length: int)
//
// so `Source` is a positional record component and reading it must yield the
// backing string. It did not: the component had NO lowering, so the read fell
// through every member-access path in codegen, lowered to no value, and the
// enclosing statement emitted NOTHING AT ALL — not even the line terminator.
// The visible symptom was the #3079 internal warning
//
//     warning: internal: the argument of this `Console.WriteLine` failed to
//     lower to a value, so the whole statement emits nothing — not even the
//     line terminator (stdlib §2.1 / §21.9) — #3079
//
// and a program printing `s.Start`, `s.Length`, `s.Source.Length` and
// `s.Start + s.Length + s.Source.Length` printed `1`, `3` and then NOTHING —
// silent wrong output, not a rejection.
//
// THE FIX (two halves, both needed):
//
//  1. codegen (`src/codegen/LlvmBackend_Exprs.cpp`): `Source` is field 0 of
//     the span's `{ptr source, i32 start, i32 length}` struct (the
//     `bz.arrayspan` layout `arraySpanStructType()` gives every span-shaped
//     record), the same field the span's own `Slice` carries forward — so a
//     sub-view's `Source` is the ORIGINAL source, exactly as §3.3's `Slice`
//     body spells it (`new ReadOnlyStringSpan(Source, this.Start + start,
//     length)`).
//
//  2. sema (`src/sema/ExprTypecheck_Calls.cpp`): the component now carries its
//     DECLARED type. Before the fix `s.Source` came out of sema with NO type
//     at all (the span record has no user TypeSymbol, and only `Length` /
//     `IsEmpty` were typed), so every use of it was unchecked — and an
//     arithmetic use reached codegen as a POINTER operand and produced
//     MALFORMED IR (`add ptr %sp.source, i32 1` → "IR verification failed"),
//     a compiler crash where a diagnostic belongs. With the type in place
//     `s.Source + 1` is a §4.6 REJECT ("`+` requires both operands to be
//     `string`") and `s.Source + "x"` is the string concatenation §4.6
//     specifies.
//
// §3.3's `Source` is a runtime-managed `string`, so the read carries NO
// disposal obligation: §9.7 classifies a record whose only reference-typed
// field is a runtime-managed `string` as NOT borrow-bearing, and `owned
// string` is inadmissible (§28.1).

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"          // runAot / runAotOut / runAotChecked
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// The issue's own repro, verbatim, with the EXPECTED output the issue names
// (`1`, `3`, `5`, `9`). On the unfixed compiler the last two lines are absent
// from stdout entirely — the statements emit nothing, not even a newline — so
// the stdout comparison is the discriminator (the exit code is 0 either way).
std::string issueRepro() {
    return
        "module Conf; "
        "public class Program "
        "  public static method Main() "
        "    var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "    Console.WriteLine(s.Start); "
        "    Console.WriteLine(s.Length); "
        "    Console.WriteLine(s.Source.Length); "
        "    Console.WriteLine(s.Start + s.Length + s.Source.Length); "
        "  end Main; "
        "end Program;";
}

}  // namespace

// #8135 / stdlib §3.3: the issue's exact repro. `s.Start` is 1, `s.Length` is
// 3, `s.Source.Length` is 5 (the WHOLE source string "hello" — `Source` is
// the source, `Start`/`Length` locate the range inside it), and
// `s.Start + s.Length + s.Source.Length` is 9.
BEZEL_TEST(readonly_string_span_source_8135, issue_repro_prints_all_four_lines) {
    BEZEL_EXPECT_EQ(runAotOut(issueRepro()), "1\n3\n5\n9");
}

// §3.3: `Source` is the backing STRING, not the viewed range — reading it
// yields "hello" in full.
BEZEL_TEST(readonly_string_span_source_8135, source_reads_the_backing_string) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  Console.WriteLine(s.Source); "
        "  return 0; "
        "end Main; end C;"), "hello");
}

// §3.3: the source is the WHOLE string, so its Length is the source's length
// (5), not the span's Length (3). This is the row that distinguishes "the
// backing string" from "the viewed range".
BEZEL_TEST(readonly_string_span_source_8135, source_length_is_the_whole_source) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  Console.WriteLine(s.Source.Length); "
        "  return 0; "
        "end Main; end C;"), "5");
}

// §3.3: the source reaches a METHOD PARAMETER — the shape §3.3 exists for (a
// tokenizer helper that needs the whole source behind a view). The callee sees
// the full "hello".
BEZEL_TEST(readonly_string_span_source_8135, source_passed_to_a_method) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  private static method Describe(src: string): int "
        "    return src.Length; "
        "  end Describe; "
        "  public static method Main(): int "
        "    var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "    Console.WriteLine(Describe(s.Source)); "
        "    return 0; "
        "  end Main; end C;"), "5");
}

// §3.3: `Slice(start, length)` returns a view over the SAME source — §3.3's
// own body spells it `new ReadOnlyStringSpan(Source, this.Start + start,
// length)`, so the sub-view's `Source` is the ORIGINAL source ("hello"), and
// the sub-view's own Start/Length move to `this.Start + start` = 2 and
// `length` = 2.
BEZEL_TEST(readonly_string_span_source_8135, slice_keeps_the_original_source) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  var sl := s.Slice(1, 2); "
        "  Console.WriteLine(sl.Source); "
        "  Console.WriteLine(sl.Source.Length); "
        "  Console.WriteLine(sl.Start); "
        "  Console.WriteLine(sl.Length); "
        "  return 0; "
        "end Main; end C;"), "hello\n5\n2\n2");
}

// §9.6 / §3.3: the `string.AsSpan()` factory mints the same record, so its
// `Source` is the string it was made from.
BEZEL_TEST(readonly_string_span_source_8135, asspan_source_is_the_string) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var sp := \"hello\".AsSpan(); "
        "  Console.WriteLine(sp.Source); "
        "  Console.WriteLine(sp.Source.Length); "
        "  return 0; "
        "end Main; end C;"), "hello\n5");
}

// §3.3 / §4.6: the component carries its DECLARED type, so §4.6's string rule
// applies to it — a mixed string/non-string `+` is REJECTED with the §4.6
// diagnostic.
//
// Before the fix the program was ACCEPTED, not rejected: the component had no
// type at all, so the `+` rule never saw a string operand, the local degraded
// to a zero value (the #2434 backstop) and the program ran and returned 0.
// (With the codegen half of the fix ALONE the same program CRASHED the
// compiler instead — the untyped component reached the operator lowering as a
// pointer and produced malformed IR, "add ptr %sp.source, i32 1" → "IR
// verification failed" — which is why the sema half is part of the fix.)
BEZEL_TEST(readonly_string_span_source_8135, source_plus_int_is_rejected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  var q := s.Source + 1; "
        "  return q; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "requires both operands to be `string`"));
}

// §3.3 / §4.6: with the component typed, the string CONCATENATION §4.6
// specifies is admitted — `s.Source + "!"` is "hello!", not a rejection.
BEZEL_TEST(readonly_string_span_source_8135, source_plus_string_concatenates) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  Console.WriteLine(s.Source + \"!\"); "
        "  return 0; "
        "end Main; end C;"), "hello!");
}

// §3.3 / §9.7: the source is a runtime-managed `string`, so reading it carries
// NO disposal obligation and the read is safe under the memory net. The row
// loops 100× over the read (a leak or a use-after-free in the emitted code
// would surface under the ASan/LSan oracle), and the value it accumulates
// (100 × 5) pins that every iteration read the WHOLE source.
//
// `runAotChecked` is the ASan/LSan oracle (the generated code is
// ASan-instrumented and the executable is run under LeakSanitizer); a
// sanitizer failure surfaces as `ran == false`, so the row asserts `ran`
// directly rather than folding it into an exit-code sentinel.
BEZEL_TEST(readonly_string_span_source_8135, source_read_100x_loop_is_asan_clean) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"hello\", 1, 3); "
        "  var i := 0; "
        "  var sum := 0; "
        "  while i < 100 do "
        "    sum := sum + s.Source.Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(sum); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");
}
