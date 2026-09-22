// Codegen — fix #4495 (stdlib §9.6 + spec §4.6, v0.306): every codegen site
// that orders `string` values — `Array<string>.Sort()`/`.Sorted()`, the
// no-selector and selector Min()/Max()/MinOrNull()/MaxOrNull() forms, and
// `Math.Min`/`Math.Max` over strings — built a `strcmp` on each operand's raw
// `.data` pointer: byte-wise AND NUL-terminated. After fix #4487 moved
// `CompareTo`/the ordering operators/`SortedSet<string>` to the width-aware
// `bz_str_compare_cp`, these sites disagreed with all of them on the same
// input, and #4487's own commit body named `Array<string>.Sort()`/`.Sorted()`
// as a known, deliberately out-of-scope survivor.
//
// THE CONTRACT. Spec §4.6 (v0.306): "`s < t`, `s > t`, `s <= t`, `s >= t`
// perform ordinal lexicographic comparison of the code-point sequence", and
// the per-string 1/2/4-byte storage width is "invisible at the language
// level" — no comparison result may depend on it. Stdlib §9.6 (v0.306):
// ordinal, code-point-based comparison. Stdlib §5.1 defines Min()/Max() (and
// their *OrNull siblings) and the selector overloads `Min<U>`/`Max<U>` over
// `IEnumerable<T> where T: IComparable<T>`, so all of them must answer by the
// SAME relation `CompareTo` does. Stdlib §2.2's `Math.Min<T>`/`Max<T>` carry
// the identical `where T: IComparable<T>` constraint.
//
// ENUMERATION (found by `grep -n 'strcmp' src/codegen/*.cpp`, filtered to drop
// `strcasecmp` bool-literal-parsing hits, which are EQUALITY checks against
// fixed ASCII literals ("true"/"1"), not an ordering relation, and are left
// untouched here — same posture #4487 took for its six wire-name EQUALITY
// sites). Six ordering call sites, ALL in LlvmBackend_Calls.cpp:
//
//   1. Array<T>.Min()/Max() no-selector, PTR element ("mm.cmp", ~L3495-3595)
//   2. Array<T>.Min<U>/Max<U> selector, PTR U ("mm.cmp", ~L3721-3808)
//   3. Array<T>.MinOrNull()/MaxOrNull() ("mon.cmp", ~L4027-4076)
//   4. Array<string>.Sort() — the `bz$strcmp` thunk (~L4894-4920)
//   5. Array<string>.Sorted() — a SEPARATE inline copy of #4's thunk builder,
//      same internal name `bz$strcmp` (~L5170-5195; narrow #3488's own note
//      records this arm had already drifted from Sort()'s once before)
//   6. Math.Min/Math.Max/Math.Clamp over strings — the `emitStrCmp` lambda
//      ("mm.strcmp", ~L10957-10973) — NOT named in the filing issue, found by
//      this file's own search of the same grep output.
//
// All six now route through `bz_str_compare_cp` (bezel_runtime_strings.cpp),
// the SAME relation #4487 established, rather than a second/third/fourth
// spelling of "ordinal string comparison" — the drift issue #2590 records.
// `bz$strcmp`'s internal LLVM function name is UNCHANGED (only its body
// moved), so nothing that references the name by string needs to move too.
//
// TEST DATA. Reused from #4487's own measurements, chosen because byte order
// and code-point order DISAGREE for them — #4487's commit body: "every
// same-width row in the suite differed at an ASCII position, where byte order
// and code-point order agree", which is exactly why this class of defect
// survives an ASCII-only suite:
//
//   "日本" (日本) vs "日x" (日x) vs "日a" (日a) — width 2, all
//   share code point U+65E5 (日) at position 0, so byte order is decided by
//   the SECOND code point's LOW byte: 本=U+672C -> 0x2C, x=U+0078 -> 0x78,
//   a=U+0061 -> 0x61, giving the byte-wise (WRONG) order 日本,日a,日x. The
//   true code-point order compares 本(0x672C) > x(0x78) > a(0x61) at that
//   position, giving 日a,日x,日本.
//
// PRE-FIX VALUES were measured mechanically (see FAIL-WITHOUT below), not
// assumed: every row's comment states the exact value observed with
// `git checkout -- src/codegen/LlvmBackend_Calls.cpp` (the parent's `strcmp`
// thunks) rebuilt in this same tree.
//
// EMBEDDED NUL (stdlib §9.6's second, independent hazard: `strcmp` stops at
// the first zero BYTE, which is not the same thing as the first zero CODE
// POINT once code points span 1/2/4-byte storage — but this row uses WIDTH-1
// content so the two notions coincide, isolating the NUL-vs-length issue from
// the width issue #4487 already covers). `"a\u0000c"` and `"a\u0000b"` both
// contain a genuine U+0000 code point; `Array<string>` elements are stored as
// full `bz_string*` objects (never truncated — unlike the SEPARATE, more
// severe pre-existing dict-key-storage defect filed as #4499, which is NOT
// this file's concern), and the runtime pads every string's data buffer with
// a trailing width-sized zero region (bezel_runtime_strings.cpp: "+ width-
// sized NUL slot"), so reading past a genuine embedded NUL via `strcmp` does
// NOT fault — it silently truncates the COMPARISON to the substring before
// the first zero byte, which for THESE two strings is identical ("a"), so
// `strcmp` (and hence Sort()) cannot tell them apart by content and falls
// back to whatever order `bz_array_sort`'s underlying `qsort_r` happens to
// produce for a "tied" pair — unspecified, and MEASURED below to land "a\0c"
// before "a\0b", the WRONG order (c > b, so "a\0c" must sort AFTER "a\0b").
//
// FAIL-WITHOUT proven mechanically (CLAUDE.md's "Never git stash" — the fix
// was copied to /var/tmp/bezel-scratch, `git checkout --
// src/codegen/LlvmBackend_Calls.cpp` reverted to the parent's thunks, bezelc
// rebuilt, each row's `.bzl` probed via `scripts/agent_check.sh --probe
// --asan`, then the fix was restored and rebuilt again — see each row's
// comment for the measured pre-fix value). Every row below either changes its
// asserted value across that revert, or (the embedded-NUL row) changes its
// asserted SIGN/exit code — none merely re-exercises the area without
// discriminating the fix, per CLAUDE.md's "must pin THE BUG, not the
// neighbourhood".
//
// EXISTING TESTS THAT PINNED THE OLD (BYTE-WISE) ORDER: none found. Searched
// every test file referencing `.Sort()`, `.Sorted()`, `bz$strcmp`,
// `Array<string>.Min`/`Max`, and `emitStrCmp`/`Math.Min`/`Math.Max`
// (`grep -rln '\.Sort()\|\.Sorted()' tests/codegen/*.cpp` and
// `grep -rl 'bz\$strcmp\|Array<string>.*Sort\|Array<string>.*Min\|Array<string>.*Max' tests/codegen/*.cpp`,
// then `grep -nP '[^\x00-\x7F]|\\u0[2-9a-fA-F]|\\U000'` over each hit): every
// one of them uses ASCII-only (width-1) string literals for the ordered
// forms. `bz_str_compare_cp` and `strcmp` AGREE at width 1 — `bz_str_cp`
// reads `((const uint8_t*)s->data)[i]` (an unsigned byte 0-255) and POSIX
// `strcmp` also compares as `unsigned char` — so no existing row's asserted
// order changes; none needed updating.
//
// No C++ line comment lives inside any Bezel source literal below: one would
// comment out the rest of the program and the row would pass for the wrong
// reason.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return std::string("module M;\nimport System;\nclass C\n"
                       "  public static method Main(): int\n") +
           body + "\n    return 0;\n  end Main;\nend C;\n";
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Generated-code ASan for the embedded-NUL row, with the observable on
// STDOUT rather than the exit code (CLAUDE.md: "8-bit exit truncation is not
// a compiler bug... move the observable to stdout") — a wrong-length copy or
// a stray read past a non-NUL-terminated buffer surfaces here as a memory
// error, not merely a value coincidence, mirroring #2785's own ASan leg for
// `emitStrCmp`. Mirrors `runAotOut`'s trailing-newline trim.
std::string runCheckedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

}  // namespace

// ===========================================================================
// 1/6 — Array<string>.Sort() (in place). stdlib §4.4 / §9.6.
// Pre-fix (measured, `git checkout -- LlvmBackend_Calls.cpp`, rebuilt):
// "sort=日本,日a,日x," (byte order). Post-fix (this row): "sort=日a,日x,日本,"
// (code-point order) — matches #4487's own SortedSet<string> measurement on
// this exact triple.
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, sort_inplace_codepoint_order_spec_9_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\"); a.Add(\"日a\");\n"
            "    a.Sort();\n"
            "    var acc := \"\";\n"
            "    for var x in a do acc := acc + x + \",\"; end;\n"
            "    Console.WriteLine($\"sort={acc}\");")),
        "sort=日a,日x,日本,");
}

// ===========================================================================
// 2/6 — Array<string>.Sorted() (copy-returning form). stdlib §3 / §4.4 / §9.6.
// This is a SEPARATE inline thunk-builder in the source (both build the same
// internal name `bz$strcmp`, but only whichever construct runs first in a
// given module actually emits the body) — the row proves BOTH copies moved,
// not just whichever `sort_inplace` above exercises first.
// Pre-fix: "sorted=日本,日a,日x,". Post-fix: "sorted=日a,日x,日本,".
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, sorted_copy_codepoint_order_spec_9_6) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\"); a.Add(\"日a\");\n"
            "    using var b := a.Sorted();\n"
            "    var acc := \"\";\n"
            "    for var x in b do acc := acc + x + \",\"; end;\n"
            "    Console.WriteLine($\"sorted={acc}\");")),
        "sorted=日a,日x,日本,");
}

// ===========================================================================
// CROSS-CHECK (the property the filing issue asks to make concrete): Sort()
// and SortedSet<string> must agree on the SAME element set. Both now route
// through bz_str_compare_cp, so they cannot drift independently again.
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, sort_agrees_with_sortedset_spec_4_18) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\"); a.Add(\"日a\");\n"
            "    a.Sort();\n"
            "    var acc := \"\";\n"
            "    for var x in a do acc := acc + x + \",\"; end;\n"
            "    using var s := new SortedSet<string>();\n"
            "    s.Add(\"日本\"); s.Add(\"日x\"); s.Add(\"日a\");\n"
            "    var acc2 := \"\";\n"
            "    for var x in s do acc2 := acc2 + x + \",\"; end;\n"
            "    Console.WriteLine($\"agree={int(acc = acc2)}\");")),
        "agree=1");
}

// ===========================================================================
// 3/6 — Array<string>.Min()/Max(), no selector. stdlib §5.1 / §9.6.
// Seeds the accumulator with element[0]="日本" and compares "日x" against it.
// Pre-fix (measured): "min=日本 max=日x" (WRONG both ways: byte-wise CompareTo
// answered positive for 日x vs 日本, so Min() never replaced its seed and
// Max() wrongly did). Post-fix: "min=日x max=日本" (日x < 日本 by code point,
// since 本=U+672C > x=U+0078 at the second position).
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, min_max_noselector_codepoint_order_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\");\n"
            "    var mn := a.Min();\n"
            "    var mx := a.Max();\n"
            "    Console.WriteLine($\"min={mn} max={mx}\");")),
        "min=日x max=日本");
}

// ===========================================================================
// 4/6 — Array<string>.Min<U>/Max<U>(selector), U = string (identity
// projection). stdlib §5.1's SELECTOR overload — a distinct codegen arm from
// row 3's no-selector form (line ~3721-3808 vs ~3495-3595).
// Pre-fix (measured): "selmin=日本 selmax=日x" (same wrong shape as row 3).
// Post-fix: "selmin=日x selmax=日本".
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, min_max_selector_codepoint_order_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var words := new Array<string>();\n"
            "    words.Add(\"日本\"); words.Add(\"日x\");\n"
            "    var mn := words.Min(lambda(s) => s);\n"
            "    var mx := words.Max(lambda(s) => s);\n"
            "    Console.WriteLine($\"selmin={mn} selmax={mx}\");")),
        "selmin=日x selmax=日本");
}

// ===========================================================================
// 5/6a — Array<string>.MinOrNull(). stdlib §5.1's *OrNull sibling of Min(),
// a THIRD distinct codegen arm ("mon.cmp", ~L4027-4076).
// Pre-fix (measured): "minornull=日本" (wrong — same seed-never-replaced
// shape). Post-fix: "minornull=日x".
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, minornull_codepoint_order_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\");\n"
            "    var mn := a.MinOrNull();\n"
            "    Console.WriteLine($\"minornull={mn!}\");")),
        "minornull=日x");
}

// 5/6b — MaxOrNull() sibling. Pre-fix (measured): "maxornull=日x" (wrong).
// Post-fix: "maxornull=日本".
BEZEL_TEST(array_string_sort_codepoint_4495, maxornull_codepoint_order_spec_5_1) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"日本\"); a.Add(\"日x\");\n"
            "    var mx := a.MaxOrNull();\n"
            "    Console.WriteLine($\"maxornull={mx!}\");")),
        "maxornull=日本");
}

// ===========================================================================
// 6/6 — Math.Min<T>/Math.Max<T> over strings. stdlib §2.2, fix #2785's own
// arm ("mm.strcmp", ~L10957-10973) — found by THIS file's own `grep -n
// strcmp` search, not named in the filing issue.
// Pre-fix (measured): "mathmin=日本 mathmax=日x" (wrong). Post-fix:
// "mathmin=日x mathmax=日本".
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, math_min_max_codepoint_order_spec_2_2) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var mn := Math.Min(\"日本\", \"日x\");\n"
            "    var mx := Math.Max(\"日本\", \"日x\");\n"
            "    Console.WriteLine($\"mathmin={mn} mathmax={mx}\");")),
        "mathmin=日x mathmax=日本");
}

// ===========================================================================
// EMBEDDED NUL (stdlib §9.6's independent NUL-truncation hazard, isolated
// from the width issue above by using WIDTH-1 content). Two width-1 strings
// containing a genuine U+0000 code point at the same position, differing only
// AFTER it: "a\u0000c" and "a\u0000b" — true code-point order is
// "a\u0000b" < "a\u0000c" (b < c at position 2), so after Sort() index 0 must
// be "a\u0000b" and CompareTo(index0, index1) must be NEGATIVE.
//
// Pre-fix (measured): plain `strcmp` on the raw `.data` pointers stops at the
// shared embedded NUL for BOTH operands, so every comparison the sort makes
// between them answers 0 (tied) and `bz_array_sort`'s underlying `qsort_r`
// leaves them in an UNSPECIFIED but MEASURED order: index 0 ends up
// "a\u0000c" (the WRONG one — cmp(index0, index1) measured +1, positive).
// Post-fix: bz_str_compare_cp reads the full LENGTH (never stops at an
// embedded zero BYTE), so index 0 is "a\u0000b" and the comparison is -1.
//
// runAotChecked (generated-code ASan): a wrong-length read here would be a
// buffer over/under-read, not just a value coincidence. The sign is printed
// to stdout (1=negative, 2=zero, 3=positive — #4487's own convention) rather
// than returned, so `prog()`'s own trailing `return 0;` stays reachable.
//
// GRAMMAR NOTE (found via bisection, not a compiler defect): an `if…then…
// else…` used as a VALUE inside a `${…}` string-interpolation hole needs its
// own outer parens — `${(if … then … else …)}` — exactly as #4487's own
// `return (if r < 0 then 1 else (if r = 0 then 2 else 3));` parenthesizes the
// OUTER if too. Omitting the outer parens (which an earlier draft of this row
// did) is rejected, and the parser's diagnostic for that reports a misleading
// `1:1: expected expression` rather than pointing at the interpolation hole —
// confirmed by bisecting down to a two-line repro (`$"x={if true then 1 else
// 2}"` alone reproduces it) and confirming the SAME construct compiles fine
// as a bare `var s := (if true then 1 else 2);` and inside interpolation once
// parenthesized. Not filed as its own issue: it is a secondary, non-blocking
// diagnostic-quality nit (wrong location, not a wrong verdict — the syntax
// IS invalid) discovered while debugging this row, and is out of this
// commit's scope.
// ===========================================================================
BEZEL_TEST(array_string_sort_codepoint_4495, embedded_nul_full_length_compare_spec_9_6) {
    BEZEL_EXPECT_EQ(
        runCheckedOut(prog(
            "    using var a := new Array<string>();\n"
            "    a.Add(\"a\\u0000c\"); a.Add(\"a\\u0000b\");\n"
            "    a.Sort();\n"
            "    var cmp := a[0].CompareTo(a[1]);\n"
            "    Console.WriteLine($\"sign={(if cmp < 0 then 1 else (if cmp = 0 then 2 else 3))}\");")),
        "sign=1");
}
