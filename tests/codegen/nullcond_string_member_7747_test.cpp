// Codegen / ownership — issue #7747 (and its sibling #7130): the `?.`
// SPELLING of the stdlib §9.6 string instance methods.
//
// SPEC. §7.5 (v0.355, docs/Bezel_Language_Spec.md:1043): "`x?.M` returns null
// if `x` is null; otherwise it performs the access. The result type is always
// nullable". §7.5's *Evaluation* paragraph (v0.355, :1051): "`?.`
// short-circuits. The receiver is evaluated first; if it is null the access
// does not occur and nothing to its right in the chain is evaluated — no
// member is read, no call is performed, and the argument and index
// expressions of the skipped suffixes are not evaluated either."
// stdlib §9.6 (v0.355, docs/Bezel_Standard_Library.md:5368) declares the
// `String` class and its instance methods.
//
// THE DEFECT. The `?.` spelling of every §9.6 string instance method was
// REFUSED with "string member `?.X` is not yet supported in codegen" — the
// diagnostic's own wording says the refusal is an implementation gap reported
// as a compile error, not a rule of the language, and §7.5 has no type-based
// exception. The `.` twin of the same members worked on a narrowed string.
//
// THE FIX. Sema admits the `?.` CALL spelling of the §9.6 instance members
// (the ONE admission list, derived from the same §9.6 instance-member table
// the `.` form's arity gate reads). The lowering is the EXISTING null-
// conditional builtin path: the receiver is lowered ONCE, a null receiver
// yields null without the call, and the non-null path runs the `.` twin's own
// `tryLowerStringMethod` and merges the result with a null pointer.
//
// THE ORACLES. Every row asserts the PRINTED OUTPUT — the value, not just
// "it compiled". A silent wrong value (the #2434/#3079 degraded-slot shape)
// exits 0, so "it ran" is not an oracle; the exact stdout is.
//
// The leak row is on `runAotChecked` (ASan/LSan) and loops 100×: a
// string-returning §9.6 method mints an OWNED string (§4.6 / §28.9), and the
// `?.` result is that owned `string?`. Before the fix the `?.` result's
// producer reference was never discharged — the consumer retained on top of
// the producer's +1 and the producer's original +1 never died (MEASURED on the
// fix's parent: 3200 byte(s) in 100 objects + 600 byte(s) indirect, the
// `bz_str_case` result of `ToUpper`, for a 100-iteration loop).

#include <string>
#include <utility>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A program with one nullable string receiver (`s`, non-null), one null
// receiver (`n`), and a `StringBuilder` trace the argument/receiver rows use.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method T(sb: StringBuilder, tag: string, v: int): int "
           "    sb.Append(tag); "
           "    return v; "
           "  end T; "
           "  public static method Pick(sb: StringBuilder, tag: string, t: string?): string? "
           "    sb.Append(tag); "
           "    return t; "
           "  end Pick; "
           "  public static method Main(): int "
           "    var s: string? := \"abcde\"; "
           "    var n: string? := null; "
           "    using var sb := new StringBuilder(); "
           "    using var sb2 := new StringBuilder(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// A compile-time rejection, told from a crash by the diagnostic text.
bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// (A) The value rows — §7.5's rule for a null and a non-null receiver, over
//     the §9.6 instance members (stdlib §9.6's Modification group and the
//     Substring group).
// ===========================================================================

// §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
// access." Each member is read on a NON-NULL receiver (its §9.6 answer) and on
// a NULL receiver (null). Eight members × two receivers. The answers are the
// `.` spelling's answers — `Remove(startIndex)` removes from `startIndex` to
// the end (stdlib §9.6's `Remove(startIndex: int): string`), so
// "abcde".Remove(1) = "a".
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_instance_methods_null_and_non_null_receiver) {
    auto r = runAot(prog(
        "    Console.WriteLine(s?.ToUpper() ?? \"NULL\"); "
        "    Console.WriteLine(n?.ToUpper() ?? \"NULL\"); "
        "    Console.WriteLine(s?.ToLower() ?? \"NULL\"); "
        "    Console.WriteLine(n?.ToLower() ?? \"NULL\"); "
        "    Console.WriteLine(s?.Trim() ?? \"NULL\"); "
        "    Console.WriteLine(n?.Trim() ?? \"NULL\"); "
        "    Console.WriteLine(s?.Substring(1) ?? \"NULL\"); "
        "    Console.WriteLine(n?.Substring(1) ?? \"NULL\"); "
        "    Console.WriteLine(s?.Substring(1, 3) ?? \"NULL\"); "
        "    Console.WriteLine(n?.Substring(1, 3) ?? \"NULL\"); "
        "    Console.WriteLine(s?.Remove(1) ?? \"NULL\"); "
        "    Console.WriteLine(n?.Remove(1) ?? \"NULL\"); "
        "    Console.WriteLine(s?.Remove(1, 2) ?? \"NULL\"); "
        "    Console.WriteLine(n?.Remove(1, 2) ?? \"NULL\"); "
        "    Console.WriteLine(s?.PadLeft(7) ?? \"NULL\"); "
        "    Console.WriteLine(n?.PadLeft(7) ?? \"NULL\"); "
        "    Console.WriteLine(s?.PadLeft(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(n?.PadLeft(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(s?.PadRight(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(n?.PadRight(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(s?.PadCenter(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(n?.PadCenter(7, '*') ?? \"NULL\"); "
        "    Console.WriteLine(s?.TrimStart() ?? \"NULL\"); "
        "    Console.WriteLine(n?.TrimStart() ?? \"NULL\"); "
        "    Console.WriteLine(s?.TrimEnd() ?? \"NULL\"); "
        "    Console.WriteLine(n?.TrimEnd() ?? \"NULL\"); "
        "    Console.WriteLine(s?.Insert(1, \"Z\") ?? \"NULL\"); "
        "    Console.WriteLine(n?.Insert(1, \"Z\") ?? \"NULL\"); "
        "    Console.WriteLine(s?.Replace(\"b\", \"X\") ?? \"NULL\"); "
        "    Console.WriteLine(n?.Replace(\"b\", \"X\") ?? \"NULL\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "ABCDE\n"
                    "NULL\n"
                    "abcde\n"
                    "NULL\n"
                    "abcde\n"
                    "NULL\n"
                    "bcde\n"
                    "NULL\n"
                    "bcd\n"
                    "NULL\n"
                    "a\n"
                    "NULL\n"
                    "ade\n"
                    "NULL\n"
                    "  abcde\n"
                    "NULL\n"
                    "**abcde\n"
                    "NULL\n"
                    "abcde**\n"
                    "NULL\n"
                    "*abcde*\n"
                    "NULL\n"
                    "abcde\n"
                    "NULL\n"
                    "abcde\n"
                    "NULL\n"
                    "aZbcde\n"
                    "NULL\n"
                    "aXcde\n"
                    "NULL\n");
}

// The bool- and int-returning §9.6 members: the `?.` result is a NULLABLE
// value (`bool?` / `int?`), so the row reads it through `??` — §7 forbids
// printing a `T?` directly.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_bool_and_int_returning_members) {
    auto r = runAot(prog(
        "    Console.WriteLine(s?.Contains(\"bc\") ?? false); "
        "    Console.WriteLine(n?.Contains(\"bc\") ?? false); "
        "    Console.WriteLine(s?.StartsWith(\"ab\") ?? false); "
        "    Console.WriteLine(n?.StartsWith(\"ab\") ?? false); "
        "    Console.WriteLine(s?.EndsWith(\"de\") ?? false); "
        "    Console.WriteLine(n?.EndsWith(\"de\") ?? false); "
        "    Console.WriteLine(s?.Equals(\"abcde\") ?? false); "
        "    Console.WriteLine(n?.Equals(\"abcde\") ?? false); "
        "    Console.WriteLine(s?.IndexOf(\"c\") ?? -1); "
        "    Console.WriteLine(n?.IndexOf(\"c\") ?? -1); "
        "    Console.WriteLine(s?.LastIndexOf(\"c\") ?? -1); "
        "    Console.WriteLine(n?.LastIndexOf(\"c\") ?? -1); "
        "    Console.WriteLine(s?.CompareTo(\"abcde\") ?? -99); "
        "    Console.WriteLine(n?.CompareTo(\"abcde\") ?? -99); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "True\nFalse\nTrue\nFalse\nTrue\nFalse\nTrue\nFalse\n"
                    "2\n-1\n2\n-1\n0\n-99\n");
}

// §7.5: "The result type is always nullable" — the `?.` result can be STORED
// in a `string?` local, tested for null, and read back.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_result_is_nullable) {
    auto r = runAot(prog(
        "    var a: string? := s?.ToUpper(); "
        "    var b: string? := n?.ToUpper(); "
        "    if a <> null then Console.WriteLine(a); end if; "
        "    if b = null then Console.WriteLine(\"null\"); end if; "
        "    Console.WriteLine(s?.Substring(1)?.Length ?? -1); "
        "    Console.WriteLine(n?.Substring(1)?.Length ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ABCDE\nnull\n4\n-1\n");
}

// ===========================================================================
// (B) §7.5's EVALUATION rule — the receiver runs once, and the ARGUMENT of a
//     skipped suffix does NOT run.
// ===========================================================================

// §7.5: "The receiver is evaluated first" — exactly once. The receiver
// expression appends its tag to the trace, so a second evaluation would show
// up as a second tag.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_receiver_is_evaluated_exactly_once) {
    auto r = runAot(prog(
        "    Console.WriteLine(Pick(sb, \"R\", s)?.ToUpper() ?? \"NULL\"); "
        "    Console.WriteLine(\"[\" + sb.ToString() + \"]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ABCDE\n[R]\n");
}

// §7.5: "the argument and index expressions of the skipped suffixes are not
// evaluated either" — a null receiver means the argument never runs, so the
// trace stays EMPTY. The non-null twin proves the argument DOES run there
// (the row is not vacuous).
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_argument_of_a_skipped_suffix_is_not_evaluated) {
    auto r = runAot(prog(
        "    Console.WriteLine(n?.PadLeft(T(sb, \"B\", 7)) ?? \"NULL\"); "
        "    Console.WriteLine(\"[\" + sb.ToString() + \"]\"); "
        "    Console.WriteLine(s?.PadLeft(T(sb2, \"B\", 7)) ?? \"NULL\"); "
        "    Console.WriteLine(\"[\" + sb2.ToString() + \"]\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "NULL\n[]\n  abcde\n[B]\n");
}

// ===========================================================================
// (C) §22.5 — the NAMED-argument spelling (the issue's own repro used named
//     arguments) and the arity gate.
// ===========================================================================

// The issue's repro spelling: `?.Remove(length := ..., startIndex := ...)`.
// §22.5: naming an argument changes which parameter it binds to, never when
// it runs — so the named form must give the same answer as the positional
// one.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_22_5_named_arguments_bind_the_same_parameters) {
    auto r = runAot(prog(
        "    Console.WriteLine(s?.Remove(length := 2, startIndex := 1) ?? \"NULL\"); "
        "    Console.WriteLine(n?.Remove(length := 2, startIndex := 1) ?? \"NULL\"); "
        "    Console.WriteLine(s?.Remove(startIndex := 1, length := 2) ?? \"NULL\"); "
        "    Console.WriteLine(s?.PadLeft(totalWidth := 7, paddingChar := '*') ?? \"NULL\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ade\nNULL\nade\n**abcde\n");
}

// §22.5 / stdlib §9.6: a wrong arity is a compile error, and the `?.`
// spelling says the SAME thing as the `.` spelling — one mistake, one
// diagnostic (before the fix the `?.` spelling said "not yet supported in
// codegen" instead).
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_22_5_wrong_arity_is_rejected_on_both_spellings) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(s?.ToUpper(1) ?? \"NULL\"); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "the call to `ToUpper` on `string` supplies "
                                  "1 argument(s) but stdlib §9.6 declares "
                                  "`ToUpper(): string`"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));

    JitResult r2 = runAot(prog(
        "    var t: string := \"ab\"; "
        "    Console.WriteLine(t.ToUpper(1)); "));
    BEZEL_EXPECT_FALSE(r2.ran);
    BEZEL_EXPECT_TRUE(hasError(r2, "the call to `ToUpper` on `string` supplies "
                                   "1 argument(s) but stdlib §9.6 declares "
                                   "`ToUpper(): string`"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r2));
}

// ===========================================================================
// (D) The LEAK row — the `?.` result is an OWNED string (§4.6 / §28.9), so
//     the compiler discharges it. On `runAotChecked` (ASan/LSan), 100
//     iterations, both a null and a non-null receiver.
// ===========================================================================

BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_28_9_the_qualified_result_is_discharged_no_leak) {
    auto r = runAotChecked(prog(
        "    var count := 0; "
        "    var i := 0; "
        "    while i < 100 do "
        "      var a := s?.ToUpper(); "
        "      var b := n?.ToUpper(); "
        "      var c := s?.Substring(1, 3); "
        "      var d := s?.PadLeft(9, '*'); "
        "      if a <> null then count := count + 1; end if; "
        "      if b = null then count := count + 1; end if; "
        "      if c = \"bcd\" then count := count + 1; end if; "
        "      if d = \"****abcde\" then count := count + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(count); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}

// ===========================================================================
// (E) CONTROLS — the fix must not widen the `?.` string gate beyond the
//     §9.6 instance members, and must not touch the `.` spelling.
// ===========================================================================

// A name §9.6 does not declare is still refused, with the `?.` spelling's own
// wording (the `.` spelling's wording differs only in the operator character).
BEZEL_TEST(codegen_nullcond_string_member_7747,
           spec_7_5_unknown_member_is_still_rejected) {
    JitResult r = runAot(prog(
        "    var z := s?.Bogus; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "string member `?.Bogus` is not yet "
                                  "supported in codegen"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// `Repeat` is a STATIC §9.6 member (stdlib §9.6, v0.350, the #7131 / #7706
// ruling), so the INSTANCE spelling is not a member of `string` — on either
// spelling. The `?.` spelling keeps the `?.` wording.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           stdlib_9_6_static_only_member_is_still_rejected) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(s?.Repeat(50) ?? \"NULL\"); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "string member `?.Repeat` is not yet "
                                  "supported in codegen"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The `.` spelling is untouched: the same member on a narrowed string still
// gives the §9.6 answer.
BEZEL_TEST(codegen_nullcond_string_member_7747,
           stdlib_9_6_dot_spelling_is_untouched) {
    auto r = runAot(prog(
        "    var t: string := \"abcde\"; "
        "    Console.WriteLine(t.ToUpper()); "
        "    Console.WriteLine(t.Substring(1, 3)); "
        "    Console.WriteLine(t.PadLeft(7, '*')); "
        "    Console.WriteLine(t.Remove(1, 2)); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ABCDE\nbcd\n**abcde\nade\n");
}
