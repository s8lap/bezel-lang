// Fix #7492 -- `s.Trim("a")` read a `string` argument as an `Array<char>`
// and crashed (stdlib §9.6, v0.350; spec §4.6, §28.9, v0.350).
//
// THE DEFECT. stdlib §9.6 declares `Trim(chars: IEnumerable<char>): string`
// and states that `string` implements `IEnumerable<char>`, so a `string` is an
// admissible `chars` argument and sema (correctly) admits it. The lowering
// handed the argument straight to the runtime's `bz_str_trim_chars`, which
// reads its set as a `bz_array_t` (an Array<char> of i32 code points). A
// `string` is a `bz_string_struct`: the helper read a string header field as
// the array count and ran off the end of the string -- SIGSEGV (rc=139) for
// `"abca".Trim("a")`, measured on the base for every string spelling (literal,
// local, concatenation).
//
// THE FIX. The lowering selects a string-set runtime entry
// (`bz_str_trim_chars_str`) by the argument's STATIC type; the set is walked
// in the code-point domain, so neither the subject's nor the set's storage
// width (§4.6: 1/2/4 bytes per code point, chosen per string) is observable.
// The `Array<char>` form keeps the old entry unchanged.
//
// THE FAMILY, MEASURED. `Trim` is the only §9.6 member whose parameter is an
// `IEnumerable<char>`; `TrimStart`/`TrimEnd` declare no parameter (sema
// refuses an argument by arity). Of the `IEnumerable<char>` argument shapes
// sema admits, only the `string` crashed; `Array<char>` is the control row.
//
// ORACLE. `runAotChecked` (generated-code ASan + LSan); every value row
// asserts the CONTENT on stdout, never only a length. The leak row loops 300x.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

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

}  // namespace

// §9.6 -- the literal repro of #7492 (ASCII subject, ASCII set).
BEZEL_TEST(string_trim_chars_string_arg_7492, issue_repro_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    var s := \"abc\" + \"a\";\n"
        "    Console.WriteLine(s.Trim(\"a\"));\n"
        "    Console.WriteLine(\"abca\".Trim(\"a\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bc\nbc");
}

// §9.6 + §4.6 -- the subject and the set each have their own width; every
// pairing is the code-point sequence, not the byte sequence.
BEZEL_TEST(string_trim_chars_string_arg_7492, mixed_widths_spec_4_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(\"aabxbaa\".Trim(\"ab\"));\n"                  // 1 / 1
        "    Console.WriteLine(\"\xe6\x97\xa5xx\xe6\x97\xa5\".Trim(\"\xe6\x97\xa5\"));\n"  // 2 / 2
        "    Console.WriteLine(\"\xf0\x9f\x98\x80\" + \"a\" + \"\xf0\x9f\x98\x80\");\n"
        "    Console.WriteLine((\"\xf0\x9f\x98\x80\" + \"a\" + \"\xf0\x9f\x98\x80\").Trim(\"\xf0\x9f\x98\x80\"));\n"  // 4 / 4
        "    Console.WriteLine(\"\xc3\xa9\" + \"a\" + \"\xc3\xa9\");\n"
        "    Console.WriteLine((\"\xc3\xa9\" + \"a\" + \"\xc3\xa9\").Trim(\"\xc3\xa9\"));\n"  // Latin-1
        "    Console.WriteLine(\"\xe6\x97\xa5" "a\xe6\x97\xa5\xc3\xa9\".Trim(\"\xc3\xa9\xe6\x97\xa5\"));\n"  // 2 / 2 (set mixes)
        "    Console.WriteLine(\"\xf0\x9f\x98\x80x\xf0\x9f\x98\x80\".Trim(\"a\xf0\x9f\x98\x80\"));\n"  // 4 / 4
        "    Console.WriteLine(\"xay\".Trim(\"\xe6\x97\xa5\xf0\x9f\x98\x80\").Length);"));       // 1 subj, wide set
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r),
                    "x\n"
                    "xx\n"
                    "\xf0\x9f\x98\x80" "a\xf0\x9f\x98\x80\n"
                    "a\n"
                    "\xc3\xa9" "a\xc3\xa9\n"
                    "a\n"
                    "a\n"
                    "x\n"
                    "3");
}

// §9.6 -- an empty set strips nothing; a set covering the subject empties it.
BEZEL_TEST(string_trim_chars_string_arg_7492, empty_set_and_full_cover_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    Console.WriteLine(\"abc\".Trim(\"\").Length);\n"
        "    Console.WriteLine(\"aaa\".Trim(\"a\").Length);\n"
        "    Console.WriteLine(\"\".Trim(\"a\").Length);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "3\n0\n0");
}

// §7.4 -- a `string?` narrowed to `string` is a `string`.
BEZEL_TEST(string_trim_chars_string_arg_7492, narrowed_nullable_set_spec_7_4) {
    JitResult r = runAotChecked(prog(
        "    var nn: string? := \"a\";\n"
        "    if nn <> null then\n"
        "      Console.WriteLine(\"abca\".Trim(nn));\n"
        "    end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "bc");
}

// §28.3.4 / §28.9 -- a +1 PRODUCER set (a concatenation, a string-returning
// call) is discharged by the caller. Loop 300x; the content is read, so a
// misread set cannot give the expected total.
BEZEL_TEST(string_trim_chars_string_arg_7492, producer_set_no_leak_spec_28_9) {
    JitResult r = runAotChecked(prog(
        "    var i := 0;\n"
        "    var n := 0;\n"
        "    while i < 300 do\n"
        "      n := n + (\"\xe6\x97\xa5" "a\" + i.ToString()).Trim(Mk(\"\xe6\x97\xa5\" + \"a\")).Length;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);"));
    BEZEL_EXPECT_TRUE(r.ran);
    // the digits of i remain: 10*1 + 90*2 + 200*3 = 790.
    BEZEL_EXPECT_EQ(stdoutOf(r), "790");
}

// The CONTROL: the `Array<char>` form keeps working exactly as it did.
BEZEL_TEST(string_trim_chars_string_arg_7492, array_char_form_unchanged_control_spec_9_6) {
    JitResult r = runAotChecked(prog(
        "    var arr: Array<char> := ['a', 'b'];\n"
        "    Console.WriteLine(\"abxba\".Trim(arr));\n"
        "    var w: Array<char> := ['\xe6\x97\xa5'];\n"
        "    Console.WriteLine((\"\xe6\x97\xa5\" + \"x\" + \"\xe6\x97\xa5\").Trim(w));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(stdoutOf(r), "x\nx");
}
