// Fix #7425 — the §9.6 string TRANSFORM surface, in the CODE-POINT domain
// (spec §4.6, v0.345; stdlib §9.6).
//
// THE DEFECT. A `string` stores each CODE POINT in `width` bytes — 1, 2 or 4,
// chosen per string from its largest code point (§4.6: "typically 1, 2, or 4
// bytes per code point, selected at construction based on the largest code
// point in the value"). §4.6 (v0.345) also says the width is NOT observable:
// "The choice of width is invisible at the language level; programs observe
// only the `char` sequence and its length."
//
// The codegen arms for the §9.6 transforms worked in the BYTE domain instead:
// they allocated `len + 1` BYTES for a string whose data is `len * width`
// bytes, copied `len` bytes (not `len * width`), compared / folded / trimmed
// one BYTE at a time, and stored width = 1. Two consequences, both measured
// on the base (see FAIL-WITHOUT below):
//
//   * `Remove` allocated `resultLen + 1` bytes but wrote `resultLen * width`
//     of them — an ASan heap-buffer-overflow WRITE on every width >= 2. A
//     WRITE, so it can corrupt the heap (not merely mis-read).
//   * the other members produced a string whose CONTENT is the BYTE
//     sequence, not the CODE-POINT sequence — the wrong TEXT at the right
//     LENGTH, which a length-only oracle cannot see.
//
// THE DOMAIN, MEASURED (this file's rows are the measurement). The overflow
// is confined to `Remove`, and it needs width >= 2:
//
//   | width | example                      | `Remove` | the transforms        |
//   |--------+-----------------------------+----------+-----------------------|
//   | 1      | "abc", "éa" (é = U+00E9)    | CLEAN    | CLEAN                 |
//   | 2      | "日" (U+65E5), "日"+"abc"    | OVERFLOW | WRONG CONTENT         |
//   | 4      | "😀" (U+1F600)               | OVERFLOW | WRONG CONTENT         |
//
// ⚠ THE ISSUE's OWN MINIMAL REPRO IS **WIDTH 1** AND DOES NOT OVERFLOW.
// #7425's body gives `var s := "éa"; var t := s.Remove(1, 1);` and reports a
// "WRITE of size 1 … 0 bytes after 2-byte region". `é` is U+00E9 = 233, which
// fits in ONE byte per code point, so "éa" is a WIDTH-1 string: the base's
// `Remove` allocates `1 + 1 = 2` bytes and writes 1 byte of terminator at
// offset 1 — inside the allocation. MEASURED on the base with
// `scripts/agent_check.sh --probe --asan`: that exact program exits 0 and
// prints "é". The "2-byte region" in the report is the RESULT's buffer, whose
// size `resultLen + 1` is a function of the RESULT length, not of the width —
// so the report is consistent with a WIDTH-2 source, and the issue's TITLE
// ("the width-2 `string` surface") is right while its REPRO is width 1.
//
// The issue's own second measurement — `var s: string := "日" + "abc";
// s.Remove(0,1)` -> "WRITE of size 6, 0 bytes after a 4-byte region" — IS a
// width-2 source, and it IS reproduced by row 1 below. And the issue's own
// note ("A WIDTH-2 STRING IS REACHABLE FROM ORDINARY CODE, BECAUSE A RANGE
// SLICE PRESERVES THE SOURCE's WIDTH") is why row 2 exists: it is the issue's
// LITERAL "é" text, in a width-2 string.
//
// THE ORACLE IS THE CONTENT, NEVER THE LENGTH (the issue's own point: "ON A
// WIDTH-2 STRING, `s.Replace('a','Z')`, `s.ToUpper()` AND `s.Trim()` ALL
// PRODUCE WRONG CONTENT WHILE `s.Length` STAYS RIGHT"). Every value row below
// asserts the program's STDOUT — the exact characters — plus `ran` and "no
// diagnostics". No row asserts the exit code, and no row asserts a length.
//
// The harness is `runAotChecked`: the AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec) with the GENERATED code
// compiled under AddressSanitizer and the program run under LeakSanitizer.
// The generated-code ASan is what turns the `Remove` heap-buffer-overflow
// WRITE into a row failure (an ASan report at the access); LSan is what
// makes the leak row a verdict rather than a report.
//
// FAIL-WITHOUT (CLAUDE.md: "Pin THE bug" / "Prove fail-without mechanically").
// The fix was reverted with `git checkout -- src/codegen/LlvmBackend_String.cpp
// src/codegen/LlvmBackend_Calls2.cpp src/runtime/bezel_runtime_strings.cpp`
// (never `git stash` — the refs/stash is shared across worktrees), the TEST
// BINARY rebuilt (`scripts/agent_check.sh` rebuilds it; rebuilding only
// `bezelc` would test the old compiler), and the rows re-run:
//
//     PRE-FIX : 1 passed, 16 failed, 21650 filtered, 21667 total
//     POST-FIX: 17 passed, 0 failed, 21650 filtered, 21667 total
//
// The ONE pre-fix pass is the CONTROL row (labelled as such below); every
// value row FAILS on the pre-fix arm. The three `Remove` rows fail with
// `ran == false` — the generated-code AddressSanitizer aborts the program on
// the out-of-bounds WRITE, which is what makes them memory-safety rows and
// not merely wrong-value rows. The pre-fix values quoted per row below are
// MEASURED (the base's stdout, byte for byte), not assumed.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "support/codegen_support.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The module every row compiles. `Console.WriteLine` puts the observable on
// STDOUT (CLAUDE.md: the exit code is 8-bit and is not a value oracle).
std::string prog(const std::string& body) {
    return std::string("module M;\nclass C\n"
                       "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

struct Outcome {
    bool ran = false;        // the program executed
    bool errored = false;    // the compiler rejected it, or a sanitizer fired
    std::string out;         // the program's stdout, trailing newline trimmed
};

// `runAotChecked` — the strict oracle: generated-code ASan + LSan.
Outcome run(const std::string& src) {
    JitResult r = runAotChecked(src);
    Outcome o;
    o.ran = r.ran;
    o.errored = r.diagnostics.hasErrors();
    std::string text = r.stdoutText;
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
        if (!text.empty() && text.back() == '\r') text.pop_back();  // CRLF
    }
    o.out = text;
    return o;
}

}  // namespace

// ===========================================================================
// 1/17 — `Remove` on a WIDTH-2 string: the out-of-bounds WRITE.
//
// The coordinator's measurement in the issue body, verbatim: "WRITE of size 6,
// 0 bytes after a 4-byte region". The base allocated `resultLen + 1 = 4`
// bytes for a result whose data is `3 * 2 = 6` bytes, then wrote 6.
//
// PRE-FIX (measured, ASan): "heap-buffer-overflow … WRITE of size 6 … 0 bytes
// after a 4-byte region" — the program aborts, so `ran` is false.
// POST-FIX: "abc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           remove_width2_mixed_write_spec_4_6) {
    Outcome o = run(prog(
        "    var s: string := \"日\" + \"abc\";\n"
        "    var t := s.Remove(0, 1);\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "abc");
}

// ===========================================================================
// 2/17 — the ISSUE's LITERAL text ("é") in a WIDTH-2 string.
//
// The issue's own note says how a width-2 string is reachable from ordinary
// code: "A RANGE SLICE PRESERVES THE SOURCE's WIDTH". So "é" — the issue's
// own repro text — reaches `Remove` at width 2 through a slice of a wide
// string. This row therefore pins the issue's REPRO in the only way it can
// fail: at width 2.
//
// PRE-FIX (measured, ASan): the base's `Remove` writes `resultLen * width`
// bytes into a `resultLen + 1` byte buffer -> "heap-buffer-overflow … WRITE
// of size 1 … 0 bytes after a 2-byte region"; the program aborts.
// POST-FIX: "é".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           remove_issue_literal_text_at_width2_write_spec_4_6) {
    Outcome o = run(prog(
        "    var w := \"日\" + \"éa\";\n"
        "    var s := w[1..2];\n"
        "    var t := s.Remove(1, 1);\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "é");
}

// ===========================================================================
// 3/17 — `Remove` on a WIDTH-4 string: the out-of-bounds WRITE.
//
// PRE-FIX (measured, ASan): "heap-buffer-overflow … WRITE of size 6 … 0 bytes
// after a 4-byte region"; the program aborts.
// POST-FIX: "abc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           remove_width4_mixed_write_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"\U0001F600\" + \"abc\";\n"
        "    var t := s.Remove(0, 1);\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "abc");
}

// ===========================================================================
// 4/17 — `Replace(oldChar, newChar)`: the CONTENT.
//
// The base compared one BYTE at a time over `len` bytes and stored width = 1,
// so on a width-2 string the result's text was the byte sequence.
//
// PRE-FIX (measured): "åeZ". POST-FIX: "日Zbc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           replace_char_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"日abc\";\n"
        "    var t := s.Replace('a', 'Z');\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日Zbc");
}

// ===========================================================================
// 5/17 — `ToUpper()`: the CONTENT.
//
// PRE-FIX (measured): "åEA". POST-FIX: "日ABC".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           toupper_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"日abc\";\n"
        "    var t := s.ToUpper();\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日ABC");
}

// ===========================================================================
// 6/17 — `ToLower()`: the CONTENT.
//
// PRE-FIX (measured): "åea". POST-FIX: "日abc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           tolower_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"日abc\";\n"
        "    var t := s.ToLower();\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日abc");
}

// ===========================================================================
// 7/17 — `Trim()`: the CONTENT.
//
// The base scanned the whitespace test one BYTE at a time, so on a width-2
// string the leading/trailing whitespace code points were not found where
// they are.
//
// PRE-FIX (measured): "" (the whole string trimmed away). POST-FIX: "日".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           trim_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \" 日 \";\n"
        "    var t := s.Trim();\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日");
}

// ===========================================================================
// 8a/17 — `TrimStart()`: the CONTENT.
//
// PRE-FIX (measured): "TrimStart" -> "" and "TrimEnd" -> " " (the base's
// byte-domain scan never found the whitespace CODE POINTS — it found the
// width-2 string's zero HIGH bytes instead, and kept only the first byte of
// each code point).
// POST-FIX: "日 " and " 日".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           trimstart_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \" 日 \";\n"
        "    var t := s.TrimStart();\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日 ");
}

BEZEL_TEST(string_width_surface_7425,
           trimend_content_spec_4_6) {  // 8b/17
    Outcome o = run(prog(
        "    var s := \" 日 \";\n"
        "    var t := s.TrimEnd();\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, " 日");
}

// ===========================================================================
// 9/17 — `Repeat(count)`: the CONTENT.
//
// The base copied `selfLen` BYTES per copy — the source's data is
// `selfLen * width` bytes — so the second copy of a width-2 string started
// in the middle of the first code point.
//
// PRE-FIX (measured): "åea". POST-FIX: "日abc日abc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           repeat_content_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"日abc\";\n"
        "    var t := String.Repeat(s, 2);\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日abc日abc");
}

// ===========================================================================
// 10a/17 — `Insert(startIndex, value)` with MIXED widths: the CONTENT.
//
// §4.6 makes the width PER STRING, so the receiver and the inserted value
// may differ. The base sized the INSERTED value with the RECEIVER's width
// (`insBytes = insLen * wid`) and copied that many bytes out of the value's
// own buffer.
//
//   * a width-2 value into a width-1 receiver: the low HALF of each code
//     point was copied. PRE-FIX (measured): "aåbc". POST-FIX: "a日bc".
//   * a width-1 value into a width-2 receiver: the value was copied as
//     `insLen * 2` bytes, reading past the value's own buffer. PRE-FIX
//     (measured): "日奘" — and the result's text is "日" followed by U+5958.
//     POST-FIX: "日XYabc".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           insert_wide_value_into_narrow_receiver_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"abc\";\n"
        "    var t := s.Insert(1, \"日\");\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "a日bc");
}

BEZEL_TEST(string_width_surface_7425,
           insert_narrow_value_into_wide_receiver_spec_4_6) {  // 10b/17
    Outcome o = run(prog(
        "    var s := \"日abc\";\n"
        "    var t := s.Insert(1, \"XY\");\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日XYabc");
}

// ===========================================================================
// 11/17 — the STATIC `string.Repeat(c: char, count: int)`: the CONTENT.
//
// The base TRUNCATED the char to its low byte and memset that byte `count`
// times into a width-1 buffer, so a code point above U+00FF was destroyed.
//
// PRE-FIX (measured): "ååå" (three U+00E5). POST-FIX: "日日日".
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           static_repeat_char_content_spec_4_6) {
    Outcome o = run(prog(
        "    var t := string.Repeat('日', 3);\n"
        "    Console.WriteLine(t);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "日日日");
}

// ===========================================================================
// 12/17 — CONTROL: the WIDTH-1 behaviour is UNCHANGED.
//
// This row PASSES on both arms — it is a control, not a discriminator: it
// pins the delta the fix is NOT allowed to have. §4.6 makes the width
// invisible, so a width-1 program's observable behaviour must be identical
// before and after. The base's byte-domain arms were correct AT WIDTH 1 (a
// width-1 string's bytes ARE its code points), and the fix keeps them so.
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           width1_behaviour_is_unchanged_control_spec_4_6) {
    Outcome o = run(prog(
        "    var s := \"abc\";\n"
        "    Console.WriteLine(s.Remove(1, 1));\n"
        "    Console.WriteLine(s.Replace('a', 'Z'));\n"
        "    Console.WriteLine(s.ToUpper());\n"
        "    Console.WriteLine(s.ToLower());\n"
        "    Console.WriteLine(\" x \".Trim());\n"
        "    Console.WriteLine(String.Repeat(s, 2));\n"
        "    Console.WriteLine(s.Insert(1, \"Y\"));\n"
        "    Console.WriteLine(string.Repeat('z', 3));\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out,
                     "ac\n"
                     "Zbc\n"
                     "ABC\n"
                     "abc\n"
                     "x\n"
                     "abcabc\n"
                     "aYbc\n"
                     "zzz");
}

// 13/17 and 14/17 (instance `GetField` / `CountOf` content rows) removed by
// fix #7746 (stdlib §9.6, v0.350; #7131 ruling): neither is a §9.6 member and
// both are rejected (tests/sema/string_instance_getfield_countof_rejected_7746).

// ===========================================================================
// 15/17 — the LEAK oracle, LOOPED.
//
// CLAUDE.md: "A LEAK REPRO MUST BE LOOPED (50-500×) — under LSan's default
// options a one-shot repro is vacuous." 200 iterations.
//
// Each result is a FRESH +1 string produced by the new width-aware runtime
// helpers (`bz_str_remove` / `bz_str_replace_char` / `bz_str_case` /
// `bz_str_trim_ws` / `bz_str_repeat` / `bz_str_insert`), so each must be
// released EXACTLY ONCE by the caller's scope boundary (§28.3 K8.1,
// §28.9). `runAotChecked` runs the program under LeakSanitizer with
// `detect_leaks=1`, so a leak FAILS this row (it is not merely reported).
//
// The observable is on STDOUT, not the exit code.
// ===========================================================================
BEZEL_TEST(string_width_surface_7425,
           transform_results_are_freed_exactly_once_lsan_spec_28_9) {
    Outcome o = run(prog(
        "    var i := 0;\n"
        "    var n := 0;\n"
        "    while i < 200 do\n"
        "      var w := \"日\" + \"abc\";\n"
        "      var a := w.Remove(0, 1);\n"
        "      var b := w.Replace('a', 'Z');\n"
        "      var c := w.ToUpper();\n"
        "      var d := w.ToLower();\n"
        "      var e := (\" 日 \").Trim();\n"
        "      var f := String.Repeat(\"日abc\", 2);\n"   // static spelling since #7706; a producer-local unit leaks: #7741

        "      var g := w.Insert(1, \"XY\");\n"
        "      var h := string.Repeat('日', 3);\n"
        "      if a.Length + b.Length + c.Length + d.Length + e.Length + f.Length + g.Length + h.Length = 33 then n := n + 1; end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(n);\n"));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_FALSE(o.errored);
    BEZEL_EXPECT_EQ(o.out, "200");
}
