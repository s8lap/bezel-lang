// Fix #7129 — the DECLARED static overload `string.Repeat(unit: string,
// count: int): string` (stdlib §9.6, v0.350; spec §4.6, §5.1, §22.5, §28.9,
// v0.350; campaign builtin-call-admission ruling R3).
//
// THE DEFECT. stdlib §9.6 declares TWO static `Repeat` overloads:
//
//     public static pure method Repeat(c: char, count: int): string;
//     public static pure method Repeat(unit: string, count: int): string;
//         // same count contract; Repeat("", n) = ""
//
// The compiler admitted only the `char` one: sema's argument-type check for
// the static `Repeat` was `requireIntLike(0, "first argument")` (#1048), so
// `string.Repeat("ab", 3)` was REFUSED with "'string.Repeat' requires a `char`
// first argument" -- a message describing the OTHER overload. A declared
// stdlib member that cannot be called is a spec deviation; the stdlib is
// normative for the surface it declares (issues #7129, see #7277, see #7495).
//
// THE FIX. Sema admits a `string` first argument (the `unit` parameter), and
// codegen lowers it through the runtime's width-aware `bz_str_repeat` -- the
// SAME helper the instance spelling uses -- behind the SHARED §9.6 count
// contract (`emitSection96CountContract`, #7169): `count < 0` raises
// ArgumentOutOfRangeException, `count = 0` is "". The `unit` argument is a
// borrowed parameter (§28.3.4), so a +1 producer argument is discharged by the
// CALLER: on the throw edge by the guard, on the normal edge after the call.
//
// WIDTHS (§4.6: "The choice of width is invisible at the language level;
// programs observe only the `char` sequence and its length."): every value
// row repeats a unit of a different storage width -- ASCII (1 byte), Latin-1
// (still 1 byte), BMP (2 bytes) and astral (4 bytes) -- and asserts the
// CONTENT on stdout, never only the length.
//
// ORACLE: `runAotChecked` -- generated-code ASan + LSan. The leak rows loop
// 300 times so LSan reliably reports a lost producer reference.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

std::string prog(const std::string& body) {
    return std::string("module M; import System;\nclass C\n"
                       "  public static method Mk(s: string): string\n"
                       "    return s + \"\";\n"
                       "  end Mk;\n"
                       "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

std::string stdoutOf(const JitResult& r) {
    std::string t = r.stdoutText;
    if (!t.empty() && t.back() == '\n') t.pop_back();
    return t;
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §9.6 -- the literal repro of #7129, ASCII unit.
BEZEL_TEST(string_repeat_string_unit_7129, ascii_unit_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat(\"ab\", 3));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "ababab");
}

// §9.6 + §4.6 -- one row per storage width, content asserted.
BEZEL_TEST(string_repeat_string_unit_7129, every_storage_width_spec_4_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat(\"\xc3\xa9\", 2));\n"            // Latin-1, width 1
        "    Console.WriteLine(string.Repeat(\"\xe6\x97\xa5x\", 3));\n"       // BMP, width 2
        "    Console.WriteLine(string.Repeat(\"\xf0\x9f\x98\x80\" + \"a\", 2));\n"  // astral, width 4
        "    Console.WriteLine(string.Repeat(\"\xf0\x9f\x98\x80\xe6\x97\xa5\" + \"a\", 2).Length);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "\xc3\xa9\xc3\xa9\n"
                    "\xe6\x97\xa5x\xe6\x97\xa5x\xe6\x97\xa5x\n"
                    "\xf0\x9f\x98\x80" "a\xf0\x9f\x98\x80" "a\n"
                    "6");
}

// §9.6 -- "Repeat("", n) = """, and `count = 0` is "".
BEZEL_TEST(string_repeat_string_unit_7129, empty_unit_and_zero_count_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat(\"\", 5).Length);\n"
        "    Console.WriteLine(string.Repeat(\"ab\", 0).Length);\n"
        "    Console.WriteLine(string.Repeat(\"\xe6\x97\xa5\", 0).Length);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "0\n0\n0");
}

// §9.6 "same count contract": a negative count raises
// ArgumentOutOfRangeException, for a literal unit and for a wide unit.
BEZEL_TEST(string_repeat_string_unit_7129, negative_count_raises_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    try\n"
        "      _ := string.Repeat(\"ab\", -1);\n"
        "    catch\n"
        "      var e: ArgumentOutOfRangeException => Console.WriteLine(\"caught-1\");\n"
        "    end try;\n"
        "    var n := -3;\n"
        "    try\n"
        "      _ := string.Repeat(\"\xe6\x97\xa5\", n);\n"
        "    catch\n"
        "      var e: ArgumentOutOfRangeException => Console.WriteLine(\"caught-2\");\n"
        "    end try;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "caught-1\ncaught-2");
}

// §5.1 -- the count is widened by the source's signedness: a `byte` 200 is
// the int 200 (not -56, which would raise).
BEZEL_TEST(string_repeat_string_unit_7129, byte_count_widens_unsigned_spec_5_1) {
    JitResult r = runAotChecked(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(string.Repeat(\"\xe6\x97\xa5\" + \"a\", b).Length);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "400");
}

// §22.5 -- a named argument binds the parameter it NAMES: `count` may come
// first. The sema check used to read `args[0]` positionally.
BEZEL_TEST(string_repeat_string_unit_7129, named_arguments_any_order_spec_22_5) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat(unit := \"\xe6\x97\xa5\xc3\xa9\", count := 2));\n"
        "    Console.WriteLine(string.Repeat(count := 2, unit := \"\xf0\x9f\x98\x80\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "\xe6\x97\xa5\xc3\xa9\xe6\x97\xa5\xc3\xa9\n"
                    "\xf0\x9f\x98\x80\xf0\x9f\x98\x80");
}

// §7.4 -- a `string?` NARROWED to `string` is a `string`.
BEZEL_TEST(string_repeat_string_unit_7129, narrowed_nullable_unit_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var sn: string? := \"\xe6\x97\xa5k\";\n"
        "    if sn <> null then\n"
        "      Console.WriteLine(string.Repeat(sn, 2));\n"
        "    end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "\xe6\x97\xa5k\xe6\x97\xa5k");
}

// §28.3.4 / §28.9 -- a +1 PRODUCER `unit` is the caller's to discharge, on the
// NORMAL edge. Loop 300x with a concatenation and a string-returning call, at
// widths 1, 2 and 4.
BEZEL_TEST(string_repeat_string_unit_7129, producer_unit_normal_edge_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    var i := 0;\n"
        "    var total := 0;\n"
        "    while i < 300 do\n"
        "      total := total + string.Repeat(\"q\" + i.ToString(), 3).Length;\n"
        "      total := total + string.Repeat(Mk(\"\xe6\x97\xa5\" + \"z\"), 2).Length;\n"
        "      total := total + string.Repeat(Mk(\"\xf0\x9f\x98\x80\" + \"z\"), 2).Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(total);"));
    BEZEL_EXPECT_TRUE(r.ran);
    // per i: len("q"+i) * 3 + 4 + 4; sum of len(i) for i in 0..299 =
    // 10*1 + 90*2 + 200*3 = 790 ⇒ 3*(300 + 790) + 300*8 = 3270 + 2400.
    BEZEL_EXPECT_EQ(stdoutOf(r), "5670");
}

// §28.9 / §31.4 -- the THROW edge releases a +1 producer `unit` too (the
// #7159 shape). The raise unwinds past the normal-path release.
BEZEL_TEST(string_repeat_string_unit_7129, producer_unit_throw_edge_no_leak_spec_28_9) {
    JitResult r = runAotChecked(
        "module M; import System;\nclass C\n"
        "  public static method Mk(s: string): string\n"
        "    return s + \"\";\n"
        "  end Mk;\n"
        "  public static method Neg(n: int): string\n"
        "    try\n"
        "      return string.Repeat(Mk(\"\xe6\x97\xa5\" + \"z\"), n);\n"
        "    catch\n"
        "      var e: ArgumentOutOfRangeException => return \"caught\";\n"
        "    end try;\n"
        "  end Neg;\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    var last := \"\";\n"
        "    while i < 300 do\n"
        "      last := Neg(-1);\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(last);\n"
        "    Console.WriteLine(Neg(2));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "caught\n\xe6\x97\xa5z\xe6\x97\xa5z");
}

// The CONTROL: the `char` overload is unchanged (and still wide-safe, #7425).
BEZEL_TEST(string_repeat_string_unit_7129, char_overload_unchanged_control_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat('x', 3));\n"
        "    Console.WriteLine(string.Repeat('\xe6\x97\xa5', 2));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "xxx\n\xe6\x97\xa5\xe6\x97\xa5");
}

// The REFUSAL side stays: a first argument that is neither a `char` nor a
// `string` is refused, and the message now names BOTH declared overloads.
BEZEL_TEST(string_repeat_string_unit_7129, double_first_argument_still_refused_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(string.Repeat(1.5, 2));"));
    BEZEL_EXPECT_TRUE(hasError(r, "'string.Repeat' requires a `char` or `string` first argument"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
