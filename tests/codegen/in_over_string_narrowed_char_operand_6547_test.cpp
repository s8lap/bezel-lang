// Regression tests for #6547 — a §7.4 flow-NARROWED `char?` left operand of
// `in` over a string (and the `string.Contains` / `IndexOf` / `LastIndexOf` /
// `Replace` / `Split` call spellings of the same search) reached the LLVM
// lowering arm still carrying its tagged-optional `{ i8, i32 }` box. The arm
// lowered it with a bare `lowerExpr` and then handed the box straight to a
// scalar-only consumer, so the compiler died in IR verification instead of
// running the program.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * spec §17.5 (v0.338): `x in coll` is the membership test; over a `string`
//     receiver it is exactly `coll.Contains(x)`. So the left operand of
//     `c in "abc"` is admitted EXACTLY when `Contains`'s `char` argument is.
//   * spec §7.4 (v0.338): flow narrowing. "an operand that §7.4 has narrowed
//     non-null is a non-nullable operand" — so after `if c is null then
//     return …; end;`, the local `c: char?` is admissible at every `char`
//     parameter and every `char` comparison. Sema does admit it, correctly.
//   * stdlib §9.6 (v0.338) declares `Contains(char)`, `IndexOf(char)`,
//     `IndexOf(char, int)`, `LastIndexOf(char)`, `Replace(char, char)` and
//     `Split(char)` on `string`.
//
// The defect is therefore in CODEGEN ONLY, and it is the `CreateICmp` row of
// the #2625 audit: the lowering must reduce the narrowed operand to its
// payload scalar, exactly as the two `Range` arms of the same `bin.op == "in"`
// block already do (`LlvmBackend_Exprs.cpp`, the `unwrapNarrowedNullableOperand`
// calls beside the §17.5 / §7.11 comments). The string-receiver arm did not.
//
// PRE-FIX, EVERY ROW BELOW DIED IN THE COMPILER (MEASURED on the base
// `0dacc5d0`, per-row diagnostics in the comments):
//
//   Both operands to ICmp instruction are not of the same type!
//     %cin.found = icmp eq i32 %cp.result, { i8, i32 } %4
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// — `ran == false` with an IR-verification diagnostic, not a wrong VALUE. So
// the rows that pin stdout are pinning the value a fix must now produce; they
// are not the oracle (the compile succeeding is). The value rows still matter:
// an unwrap that reads the wrong struct index, or a `sext` that widens the
// wrong field, would compile and print the wrong thing.
//
// EXPECTED VALUES, one row per BEZEL_TEST below (22 rows, 22 tests). Each
// value is the spec-correct answer, stated BEFORE running. Where a test prints
// more than one line the whole stdout is given, so the table is a complete
// inventory of the file — a row that names a test which does not exist would
// be a second, wrong inventory.
//
//   Category A — the DEFECT (fail without the fix; pre-fix each dies in IR
//   verification, so `ran == false` and the value column is what the fix must
//   then produce):
//     narrowed_char_left_operand_in_string             "True\n"
//     narrowed_char_absent_in_string                   "False\n"
//     narrowed_char_late_in_string                     "True\n"
//     narrowed_non_ascii_char_in_string                "True\n"
//     narrowed_char_is_null_returns_early              ""       (exit 1)
//
//   Category B — the CONTROLS (green on both sides):
//     un_narrowed_char_left_operand_control            "True\n"
//     narrowed_char_plus_coalesce_control              "False\nTrue\n"
//     string_left_operand_control                      "True\nFalse\n"
//     narrowed_char_left_operand_in_range_control      "True\nFalse\nTrue\n"
//
//   Category C — REJECTIONS the fix must NOT remove (§5.1; asserted on the
//   conversion-site diagnostic, not on stdout):
//     int_left_operand_still_rejected_spec_5_1         §5.1 error
//     double_left_operand_still_rejected_spec_5_1      §5.1 error
//     byte_left_operand_still_rejected_spec_5_1        §5.1 error
//
//   Category D — the §7 gates that make the unwrap sound (one per site the fix
//   touched; asserted on the nullable-argument diagnostic):
//     un_narrowed_char_left_operand_still_rejected_spec_7   §7 error
//     un_narrowed_contains_arg_still_rejected_spec_7        §7 error
//     un_narrowed_index_of_arg_still_rejected_spec_7         §7 error
//     un_narrowed_replace_arg_still_rejected_spec_7         §7 error
//     un_narrowed_last_index_of_arg_still_rejected_spec_7   §7 error
//     un_narrowed_split_arg_still_rejected_spec_7           §7 error
//     un_narrowed_index_of_with_start_index_arg_still_rejected_spec_7  §7 error
//
//   Category E — the CALL-SPELLING TWINS (fail without the fix):
//     string_contains_narrowed_char                   "True\nFalse\n"
//     string_index_of_narrowed_char                   "2\n-1\n"
//     string_index_of_char_with_start_index           "2\n2\n"
//     string_last_index_of_narrowed_char              "4\n-1\n"
//     string_replace_narrowed_chars                   "axc\nabc\n"
//     string_split_narrowed_char                      "a|c\n"
//
// `narrowed_char_left_operand_in_range_control` prints `'b' in 'a'..'z'` (True),
// `'b' in 'a'..'a'` (False) and `'b' in 'a'..'b'` (True). The middle one is
// False because `'b'` is NOT the bound — NOT because `a..a` is empty. Spec
// §16.3 (v0.338): "An inclusive equal-bounds range (`5..5`, `5..5 step 1`,
// `5..5 step -1`) yields exactly one value — the bound itself." MEASURED:
// `'a' in 'a'..'a'` is True and `5 in 5..5` is True; `'b' in 'a'..'a'` and
// `4 in 5..5` are both False. An inclusive equal-bounds range is a ONE-ELEMENT
// range, so the `Range` arm must NOT special-case it as empty.
//
// ORACLE DISCIPLINE.
//
//   * Every value row asserts on `stdoutText`, byte-for-byte — a bare
//     `exitCode` oracle would green a wrong answer (CLAUDE.md, "a wrong answer
//     that still exits 0"). Where a test prints several lines the whole stdout
//     is asserted, not a substring of it.
//   * Every row READS THE DATA THE FIX HANDLES: the searched string's content
//     decides the answer, and the absent cases flip it. A row whose body is a
//     constant could not see a fix that misreads its operand.
//   * The three REJECT rows (§5.1 `int` / `double` / `byte` left operands) are
//     the guard against a fix that makes REJECTED programs compile. They assert
//     the SPECIFIC §5.1 conversion-site diagnostic and assert the pre-#6542
//     codegen NYI text is ABSENT — the layer #6542 moved these to. They are
//     asserted on a program that does NOT compile, so `runAot` does fill
//     diagnostics; this is not the "assert no warning on a compiling program"
//     trap.
//   * The un-narrowed rows are the SOUNDNESS control for
//     `unwrapNarrowedNullableOperand`, which tests the tagged SHAPE, not that
//     sema proved non-nullness. Each one asserts the §7 nullable-argument
//     diagnostic, which is the sema gate that makes the unwrap sound at that
//     site. If such a row ever compiled, the unwrap would be turning a clean
//     rejection into a silent wrong value.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Wraps `body` in a runnable `Main` that returns 0.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// Same, but with an extra method `Run` declared on the class — the shape the
// SOUNDNESS rows need, since a `char?` PARAMETER cannot be flow-narrowed at
// its use site from a guard written in `Main`.
std::string progWithMethod(const std::string& sig,
                           const std::string& body,
                           const std::string& callArgs) {
    return "module M;\n"
           "class C\n" + sig + "\n" +
           "  end Run;\n"
           "  public static method Main(): int\n"
           "    using var c := new C();\n"
           "    Console.WriteLine(" + callArgs + ");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    return hasErrorContaining(r, std::string(needle));
}

// §5.1's conversion-site diagnostic, quoted verbatim (the needle #6542's fix
// put in place, and the one these rows must still produce).
const char* kConversionSite =
    "left operand of `in` over a `string` receiver has a type that is not "
    "implicitly convertible to the target type (\xC2\xA7""5.1)";
// The pre-#6542 codegen-layer text, asserted ABSENT so a regression to the
// old layer attribution is caught here too.
const char* kWrongLayer = "not yet implemented: codegen for this `in` expression";

// The §7 nullable-argument gate, quoted verbatim. Asserted by every
// un-narrowed control row: it is the sema rejection that makes the
// shape-based unwrap sound at each call site.
const char* kNullableArgIn = "the left operand of `in` over a `string` receiver "
                             "is nullable but the target type is non-nullable";

}  // namespace

// ===========================================================================
// Category A — the DEFECT: `in` over a string with a narrowed `char?` LHS
// ===========================================================================

// The issue's own repro, verbatim. §7.4 narrows `c` at the `char` position
// after the `is null` guard; §17.5 admits the left operand there because
// `Contains`'s `char` argument admits it. Pre-fix: `icmp eq i32, {i8,i32}`
// → IR verification failure, `ran == false`, no output at all. Post-fix the
// program runs and `c in "abc"` is `True`.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_left_operand_in_string) {
    auto r = runAot(prog(
        "    var c: char? := 'c';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in \"abc\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The absent case: the SAME shape, a code point absent from the haystack.
// Without it the row above could not distinguish an unwrap that reads the
// payload from one that always yields the first comparison's answer.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_absent_in_string) {
    auto r = runAot(prog(
        "    var c: char? := 'z';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in \"abc\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// The narrowed operand at a NON-FIRST position in the haystack, so a fix that
// stops at index 0 cannot pass.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_late_in_string) {
    auto r = runAot(prog(
        "    var c: char? := 'e';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in \"abcdef\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// A width-2 haystack. `loadCodePoint` decodes UTF-16 there, so the needle is
// a code point above 127 — the row nearest the `{i8,i32}` box's payload
// width, and one that would expose an unwrap reading the hasValue tag
// instead of the payload.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_non_ascii_char_in_string) {
    auto r = runAot(prog(
        "    var c: char? := '\\u00e9';\n"  // é, one code point
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in \"caf\\u00e9\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// The narrowed null operand takes the §7.4 EARLY RETURN and never reaches the
// `in`. Guards the guard: if the fix's unwrap had changed flow-narrowing
// itself, this row would print instead of exiting 1.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_is_null_returns_early) {
    auto r = runAot(prog(
        "    var c: char? := null;\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in \"abc\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// Category B — the CONTROLS: shapes that must be UNCHANGED by the fix
// ===========================================================================

// A plain, un-narrowed `char` left operand — the spelling that worked before
// and must still work. An unwrap predicate that fires on a bare `i32` (rather
// than on the `{i8,i32}` box) would break this row.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_char_left_operand_control) {
    auto r = runAot(prog(
        "    var c: char := 'b';\n"
        "    Console.WriteLine((c in \"abc\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// §7.7's `??` yields a NON-NULLABLE `char`, so no tagged slot exists. This is
// the spelling the issue's "§7.7 half" note says already worked; pinning it
// means a widening of the unwrap (or a change to `??`'s result type) is
// caught.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_plus_coalesce_control) {
    auto r = runAot(prog(
        "    var c: char? := 'z';\n"
        "    Console.WriteLine(((c ?? 'a') in \"abc\").ToString());\n"
        "    Console.WriteLine(((c ?? 'a') in \"xyz\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n");
}

// A narrowed `string?` left operand — §17.5's SUBSTRING test, a different arm
// of the same `in` block and a different payload shape. Must keep working.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_left_operand_control) {
    auto r = runAot(prog(
        "    var s: string? := \"bc\";\n"
        "    if s is null then return 1; end;\n"
        "    Console.WriteLine((s in \"abc\").ToString());\n"
        "    Console.WriteLine((s in \"xyz\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// The `Range` arms of the same `bin.op == "in"` block, which already took the
// unwrap before this fix (#3298) — the arm #6547's fix brought into line. The
// operand is the NARROWED `c: char?`, so this exercises the Range arm's own
// unwrap and would go red if the fix's predicate were broad enough to break
// the arm that already worked.
//
// ⚠ The middle expectation is `False` because `'b'` is NOT the bound, NOT
// because `'a'..'a'` is empty. Spec §16.3 (v0.338): "An INCLUSIVE
// equal-bounds range (`5..5`, `5..5 step 1`, `5..5 step -1`) yields exactly one
// value — the bound itself." MEASURED: `'a' in 'a'..'a'` is True and
// `5 in 5..5` is True; `'b' in 'a'..'a'` and `4 in 5..5` are both False. So an
// inclusive equal-bounds range is a ONE-ELEMENT range and the `Range` arm must
// NOT special-case it as empty. (Only the HALF-OPEN equal-bounds form is empty,
// also §16.3.) This row is the guard against "fixing" the arm on the strength
// of a wrong gloss.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           narrowed_char_left_operand_in_range_control) {
    auto r = runAot(prog(
        "    var c: char? := 'b';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine((c in 'a'..'z').ToString());\n"
        "    Console.WriteLine((c in 'a'..'a').ToString());\n"
        "    Console.WriteLine((c in 'a'..'b').ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

// ===========================================================================
// Category C — the REJECTIONS the fix must NOT remove
//
// #6547 made MORE programs compile (it turns an IR-verification crash into a
// running program), so the risk direction is "a rejection that now also fires
// on a valid program" AND — for a fix that instead implemented a lowering arm
// for non-`char` left operands — "REJECTED programs that now compile". These
// rows are the second half. (Sibling suite `sema/`
// in_over_string_left_operand_6542_test.cpp holds the full Category B set for
// #6542; these three are the minimum the fix's own risk direction requires.)
// ===========================================================================

// An `int` left operand. §5.1's table lists `char` as a SOURCE only and §4.2's
// integer-types table omits `char`, so integer→`char` is §5.2's EXPLICIT
// `char(n)` conversion — this is CORRECTLY REJECTED, at the conversion site.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           int_left_operand_still_rejected_spec_5_1) {
    auto r = runAot(prog("    Console.WriteLine((1 in \"abc\").ToString());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// A `double` left operand — §5.1 has no `double`→`char` row.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           double_left_operand_still_rejected_spec_5_1) {
    auto r = runAot(prog("    Console.WriteLine((1.5 in \"abc\").ToString());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// A `byte` left operand — a WIDENING integer, the row nearest §5.1's table
// and the one an over-broad predicate would let through.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           byte_left_operand_still_rejected_spec_5_1) {
    auto r = runAot(prog(
        "    var b: byte := 99;\n"
        "    Console.WriteLine((b in \"abc\").ToString());\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kConversionSite));
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kWrongLayer));
}

// ===========================================================================
// Category D — the SOUNDNESS controls for the unwrap
//
// `unwrapNarrowedNullableOperand` tests the tagged `{i8, i32}` SHAPE, not
// that sema proved the value non-null. It is sound ONLY where a sema gate
// rejects the un-narrowed form. The fix added the unwrap at SEVEN sites (the
// `in` arm plus six string-method arms), and there is ONE row per site below —
// seven rows — so the file pins every gate the fix now leans on rather than a
// sample of them. Were one of these to COMPILE, the unwrap would silently turn
// a clean rejection into a wrong value.
// ===========================================================================

// The UN-narrowed `in` left operand — the gate the string-receiver arm's
// unwrap rests on. Pre- and post-fix identical (§7), and the whole reason
// the `in` unwrap is sound.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_char_left_operand_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): bool\n"
        "    return c in \"abc\";",
        "", "c.Run('c').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kNullableArgIn));
}

// The UN-narrowed `Contains` argument.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_contains_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): bool\n"
        "    return \"abc\".Contains(c);",
        "", "c.Run('c').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.Contains` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// The UN-narrowed `IndexOf` argument.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_index_of_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): int\n"
        "    return \"abc\".IndexOf(c);",
        "", "c.Run('c').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.IndexOf` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// The UN-narrowed `Replace` argument (the `oldChar` position).
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_replace_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): string\n"
        "    return \"abc\".Replace(c, 'x');",
        "", "c.Run('b')"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.Replace` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// The UN-narrowed `LastIndexOf` argument.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_last_index_of_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): int\n"
        "    return \"abc\".LastIndexOf(c);",
        "", "c.Run('c').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.LastIndexOf` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// The UN-narrowed `Split` argument.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_split_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): int\n"
        "    using var parts := \"abc\".Split(c);\n"
        "    return parts.Count;",
        "", "c.Run('b').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.Split` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// The UN-narrowed `char` argument at the TWO-argument `IndexOf(char, int)` —
// the site whose `startIndex` operand keeps its own (separate, #2614) hole.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           un_narrowed_index_of_with_start_index_arg_still_rejected_spec_7) {
    auto r = runAot(progWithMethod(
        "  method Run(c: char?): int\n"
        "    return \"abc\".IndexOf(c, 1);",
        "", "c.Run('c').ToString()"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "the argument to `string.IndexOf` "
                                            "is nullable but the target type is "
                                            "non-nullable"));
}

// ===========================================================================
// Category E — the CALL-SPELLING TWINS of the defect
//
// §17.5 makes `c in "abc"` the call `"abc".Contains(c)`, so the two spellings
// share a left operand and must agree. The other `char`-typed string search
// and edit members are the same shape at the same layer and were equally
// broken. Every row below is MEASURED pre-fix as an IR-verification failure
// (`ran == false`), so each is a fail-without row of its own.
// ===========================================================================

// `string.Contains(c)` with a narrowed `char?` — the issue's second named
// spelling, and the one whose value must AGREE with row
// `narrowed_char_left_operand_in_string` (both `True`).
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_contains_narrowed_char) {
    auto r = runAot(prog(
        "    var c: char? := 'c';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".Contains(c).ToString());\n"
        "    Console.WriteLine(\"abc\".Contains('z').ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// `string.IndexOf(c)` — `2` for the matching case, `-1` for the absent one,
// so the row reads the data and pins both branches of the scan.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_index_of_narrowed_char) {
    auto r = runAot(prog(
        "    var c: char? := 'c';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(c).ToString());\n"
        "    var d: char? := 'z';\n"
        "    if d is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(d).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n-1\n");
}

// `string.IndexOf(c, startIndex)` — the two-`char` spelling's sibling: the
// `char` operand here was ALSO broken, at the same site as the one-arg form.
// (`startIndex` is `int`, whose own narrowing hole is the separate #2614
// int-parameter family — the plain `int` below is its control.)
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_index_of_char_with_start_index) {
    auto r = runAot(prog(
        "    var c: char? := 'c';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(c, 1).ToString());\n"
        "    Console.WriteLine(\"abc\".IndexOf('c', 0).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n2\n");
}

// `string.LastIndexOf(c)` — the backward scan.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_last_index_of_narrowed_char) {
    auto r = runAot(prog(
        "    var c: char? := 'b';\n"
        "    if c is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".LastIndexOf(c).ToString());\n"
        "    var d: char? := 'z';\n"
        "    if d is null then return 1; end;\n"
        "    Console.WriteLine(\"abcabc\".LastIndexOf(d).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n-1\n");
}

// `string.Replace(c, r)` — BOTH operands are narrowed, so the row exercises
// the unwrap on the `oldChar` comparison AND the `newChar` truncation.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_replace_narrowed_chars) {
    auto r = runAot(prog(
        "    var c: char? := 'b';\n"
        "    if c is null then return 1; end;\n"
        "    var x: char? := 'x';\n"
        "    if x is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".Replace(c, x));\n"
        "    Console.WriteLine(\"abc\".Replace('z', x));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "axc\nabc\n");
}

// `string.Split(c)` — pre-fix the arm sexted the `{i8,i32}` BOX, a different
// IR diagnostic ("SExt only operates on integer") from the `icmp` family.
BEZEL_TEST(in_over_string_narrowed_char_operand_6547,
           string_split_narrowed_char) {
    auto r = runAot(prog(
        "    var c: char? := 'b';\n"
        "    if c is null then return 1; end;\n"
        "    using var parts := \"abc\".Split(c);\n"
        "    Console.WriteLine(string.Join(\"|\", parts));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a|c\n");
}