// Fix #7485 -- `string.Join` wrote in the BYTE domain: a width-2 element
// rendered as its low byte (spec §4.6, stdlib §9.6, v0.350).
//
// spec §4.6: strings store 1, 2 or 4 bytes per code point, but "The choice of
// width is invisible at the language level; programs observe only the `char`
// sequence and its length." stdlib §9.6 declares
// `string.Join(separator: string, items: IEnumerable<string>): string`.
//
// THE DEFECT (MEASURED on the parent):
//   string.Join(",", ["日","abc"])   -> "å,abc"   (expected "日,abc")
//   string.Join("日", ["a","b"])      -> "aåb"     (expected "a日b")
//   string.Join(",", ["😀","abc"])   -> garbage   (width 4)
// The inline lowering allocated `total + 1` BYTES, `memcpy`'d each separator /
// element's code-point COUNT as a BYTE count and stored width = 1.
//
// THE FIX. The join is built by the runtime (`bz_str_join`) from each
// operand's code points, packed at the narrowest width that fits the result.
//
// The array operands are spelled `using var`: a `var` array passed to
// `string.Join` leaks (a separate defect, see string_join_items_element_width_7167_test.cpp).
//
// ORACLE. `runAotChecked` (generated-code ASan + LSan); each row asserts the
// program ran, its exit code and its exact stdout. The observable is the
// `char` sequence: the rendered text, `Length`, and the indexed code point.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return std::string(
               "module M; import System; import System.Linq;\nclass C\n"
               "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

void expectOut(const std::string& body, const std::string& want) {
    JitResult r = runAotChecked(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    BEZEL_EXPECT_EQ(t, want);
}

}  // namespace

// The issue's first repro: a width-2 ELEMENT.
BEZEL_TEST(string_join_width_domain_7485, width2_element_spec_4_6) {
    expectOut(
        "    using var a: Array<string> := [\"日\", \"abc\"];\n"
        "    var j := string.Join(\",\", a);\n"
        "    Console.WriteLine(j);\n"
        "    Console.WriteLine(j.Length);\n"
        "    Console.WriteLine(j[0] = '日');",
        "日,abc\n5\nTrue");
}

// The issue's second repro: a width-2 SEPARATOR.
BEZEL_TEST(string_join_width_domain_7485, width2_separator_spec_4_6) {
    expectOut(
        "    using var a: Array<string> := [\"a\", \"b\", \"c\"];\n"
        "    var j := string.Join(\"日\", a);\n"
        "    Console.WriteLine(j);\n"
        "    Console.WriteLine(j.Length);",
        "a日b日c\n5");
}

// The issue's third repro: a width-4 element (and a width-4 separator).
BEZEL_TEST(string_join_width_domain_7485, width4_element_and_separator_spec_4_6) {
    expectOut(
        "    using var a: Array<string> := [\"😀\", \"abc\"];\n"
        "    Console.WriteLine(string.Join(\",\", a));\n"
        "    var j := string.Join(\"😀\", a);\n"
        "    Console.WriteLine(j);\n"
        "    Console.WriteLine(j.Length);",
        "😀,abc\n😀😀abc\n5");
}

// Elements of DIFFERENT widths in one join: each is read at its own width.
BEZEL_TEST(string_join_width_domain_7485, mixed_widths_spec_4_6) {
    expectOut(
        "    using var a: Array<string> := [\"x\", \"日本\", \"😀\", \"é\", \"\"];\n"
        "    var j := string.Join(\"|\", a);\n"
        "    Console.WriteLine(j);\n"
        "    Console.WriteLine(j.Length);",
        "x|日本|😀|é|\n9");
}

// A LAZY source (the #1182 path: a LINQ query is materialized to a snapshot
// that is freed after the join) over wide elements.
BEZEL_TEST(string_join_width_domain_7485, lazy_source_wide_elements_spec_4_6) {
    expectOut(
        "    using var a: Array<string> := [\"日\", \"bb\", \"語\"];\n"
        "    Console.WriteLine(string.Join(\"・\", a.Where(lambda(s) => s.Length >= 1)));",
        "日・bb・語");
}

// A named-argument call in declaration order (the admitted spelling, #7165)
// at width 2.
BEZEL_TEST(string_join_width_domain_7485, named_arguments_wide_spec_22_5) {
    expectOut(
        "    using var a: Array<string> := [\"日\", \"語\"];\n"
        "    Console.WriteLine(string.Join(separator := \"-\", items := a));",
        "日-語");
}

// CONTROLS (ASCII, the width-1 path that already worked): empty, one element,
// ordinary elements.
BEZEL_TEST(string_join_width_domain_7485, ascii_controls_spec_9_6) {
    expectOut(
        "    using var e: Array<string> := [];\n"
        "    using var o: Array<string> := [\"solo\"];\n"
        "    using var m: Array<string> := [\"a\", \"b\", \"c\"];\n"
        "    Console.WriteLine(string.Join(\",\", e).Length);\n"
        "    Console.WriteLine(string.Join(\",\", o));\n"
        "    Console.WriteLine(string.Join(\", \", m));",
        "0\nsolo\na, b, c");
}

// Loop-amplified: the joined result (and the lazy snapshot) must not leak or be
// freed twice, looped 100x.
BEZEL_TEST(string_join_width_domain_7485, looped_no_leak_no_double_free_spec_28_9) {
    expectOut(
        "    using var a: Array<string> := [\"日\", \"abc\"];\n"
        "    var n := 0;\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      var j := string.Join(\"😀\", a.Where(lambda(s) => s.Length >= 1));\n"
        "      n := n + j.Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);",
        "500");
}
