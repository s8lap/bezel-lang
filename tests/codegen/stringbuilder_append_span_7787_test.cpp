// Regression tests for #7787 -- `sb.Append(span)` with a `ReadOnlyStringSpan`
// appended the EMPTY string: the span lowers to the {ptr,i32,i32} record value
// (Source, Start, Length; stdlib §3.3), and the StringBuilder `Append` arm's
// string coercion fell through to its "format as integer" fallback.
//
// stdlib §3.1 (v0.350), verbatim:
//   "public method Append(value: ReadOnlyStringSpan): StringBuilder;
//    // (#1810): copies the span's code points; = Append(value.ToString())
//    without the intermediate string"
//
// The oracle is stdout byte-for-byte: an empty append still exits 0. Leak rows
// run on runAotChecked (ASan + LSan) and loop 100 times.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

JitResult run(const std::string& body) {
    return runAotChecked(
        "module M; import System; import System.Text;\n"
        "class C\n"
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
}

void expectOut(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's repro (u02): a non-nullable span local.
BEZEL_TEST(stringbuilder_append_span_7787,
           non_nullable_span_is_copied_stdlib_3_1) {
    expectOut(run("    var sb := new StringBuilder();\n"
                  "    var sp: ReadOnlyStringSpan := \"hello\".AsSpan();\n"
                  "    sb.Append(sp);\n"
                  "    Console.WriteLine(sb.ToString());\n"),
              "hello\n");
}

// The issue's repro (u01): a span narrowed from a nullable local.
BEZEL_TEST(stringbuilder_append_span_7787,
           narrowed_nullable_span_is_copied_stdlib_3_1) {
    expectOut(run("    var sb := new StringBuilder();\n"
                  "    var sp: ReadOnlyStringSpan? := \"hello\".AsSpan();\n"
                  "    if sp <> null then sb.Append(sp); end if;\n"
                  "    Console.WriteLine(sb.ToString());\n"),
              "hello\n");
}

// A WINDOW of a wider string: only the span's code points are copied, and the
// builder keeps the storage width ("é" is U+00E9, "世" U+4E16, "界" U+754C).
BEZEL_TEST(stringbuilder_append_span_7787,
           window_of_wide_string_copies_only_the_window_stdlib_3_1) {
    expectOut(run("    var sb := new StringBuilder(\"<\");\n"
                  "    var src := \"aé世界z\";\n"
                  "    sb.Append(src.AsSpan(1, 3)).Append(\">\");\n"
                  "    var t := sb.ToString();\n"
                  "    Console.WriteLine(t.Length);\n"
                  "    Console.WriteLine(t[1] = 'é');\n"
                  "    Console.WriteLine(t[2] = '世');\n"
                  "    Console.WriteLine(t[3] = '界');\n"
                  "    Console.WriteLine(t[4] = '>');\n"),
              "5\nTrue\nTrue\nTrue\nTrue\n");
}

// A span built by `Slice` and appended twice, in a chain: equal to
// Append(span.ToString()).
BEZEL_TEST(stringbuilder_append_span_7787,
           chained_sliced_span_equals_append_of_tostring_stdlib_3_1) {
    expectOut(run("    var sp := \"abcdef\".AsSpan().Slice(2, 3);\n"
                  "    var a := new StringBuilder().Append(sp).Append(sp).ToString();\n"
                  "    var b := new StringBuilder().Append(sp.ToString())"
                  ".Append(sp.ToString()).ToString();\n"
                  "    Console.WriteLine(a);\n"
                  "    Console.WriteLine(a = b);\n"),
              "cdecde\nTrue\n");
}

// The empty span appends nothing; the builder is untouched.
BEZEL_TEST(stringbuilder_append_span_7787, empty_span_appends_nothing_stdlib_3_1) {
    expectOut(run("    var sb := new StringBuilder(\"ab\");\n"
                  "    sb.Append(\"xyz\".AsSpan(1, 0));\n"
                  "    Console.WriteLine(sb.ToString());\n"
                  "    Console.WriteLine(sb.Length);\n"),
              "ab\n2\n");
}

// The copied part is a fresh +1 the builder owns: 100 iterations of append,
// ToString and disposal must neither leak nor free the span's Source.
BEZEL_TEST(stringbuilder_append_span_7787, span_part_is_owned_loop_100_stdlib_3_1) {
    expectOut(run("    var src := \"hello world\";\n"
                  "    var tot := 0;\n"
                  "    for var i in 0..<100 do\n"
                  "      using var sb := new StringBuilder();\n"
                  "      sb.Append(src.AsSpan(6, 5)).Append(src.AsSpan(0, 5));\n"
                  "      tot := tot + sb.ToString().Length;\n"
                  "    end;\n"
                  "    Console.WriteLine(tot);\n"
                  "    Console.WriteLine(src);\n"),
              "1000\nhello world\n");
}
