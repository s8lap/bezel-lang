// fix #7377 (stdlib §2.18 / language spec §18.5, v0.355) — an ENUM receiver
// is rejected with TWO different diagnostics for the SAME obligation: the
// §8.1 arity message in the member spelling, the §18.5 vocabulary message in
// the hole spelling.
//
// THE DEFECT (MEASURED on r30-c1 / v0.355, commit bc9c0d60):
//
//   e.ToString("F4")   ->  "the universal `Object.ToString` member takes 0
//                           argument(s) but the call supplies 1 (§8.1)"
//   $"{e:F4}"          ->  "a format spec requires an intrinsically-formattable
//                           operand …; `E` is neither (§18.5)"
//
// ONE OBLIGATION, TWO DIAGNOSTICS. Spec §18.5's "Format dispatch" makes the
// two spellings ONE thing — a hole's format spec "dispatches to
// `ToString(format: string)`" — so the reader must get ONE answer.
//
// AND THE FIRST MESSAGE IS MISLEADING ABOUT THE CAUSE. The §8.1 message blames
// ARITY on a call whose problem is that the receiver's type has no format
// vocabulary at all.
//
// THE GOVERNING TEXT.
//
// Language spec §18.5 (docs/Bezel_Language_Spec.md, "Format dispatch"):
//
//   A hole *with* a format spec dispatches to `ToString(format: string)`: the
//   primitive and temporal types provide it intrinsically (the specifier
//   vocabulary and its invariant-culture semantics are normative in stdlib
//   §2.18); a **user** type opts in by implementing the auto-imported
//   interface `IFormattable` (stdlib §2.18). A format spec on an operand whose
//   type is neither intrinsically formattable nor `IFormattable` is a compile
//   error naming this rule.
//
// Standard library §2.18 (docs/Bezel_Standard_Library.md) — the specifier
// vocabulary's table covers the intrinsic types (the integer tower, the
// floating types, `decimal`, `DateTime`/`DateOnly`/`TimeOnly`, `TimeSpan`,
// `Guid`) and its last row gives `bool`, `char` and `string` an EMPTY one:
//
//   | `bool`, `char`, `string` | *(none)* | a format spec on these is a compile error |
//
// and §2.18's neighbouring sentence covers every other non-conforming operand:
//
//   **a format spec on any other non-conforming operand is a compile error.**
//
// AN ENUM IS NOT IN ANY VOCABULARY ROW. It is not one of the intrinsically
// formattable types, and it cannot opt in: §10.5 (the enum's member surface)
// lets an enum declare `Equals` (own-typed), `GetHashCode(): int` and
// `ToString(): string` — at ARITY ZERO — so an enum can never supply the
// `ToString(format: string)` that `IFormattable` declares (stdlib §2.18). An
// enum therefore has NO format vocabulary, and the right answer for a format
// spec on an enum is a REJECTION — not a better rejection, but ONE rejection
// for BOTH spellings.
//
// THE FIX. The admission gate now asks the question of EVERY receiver (not
// only of the receivers that carry their own `ToString` overloads): "does this
// type have a format vocabulary?" — from the receiver's DECLARED TYPE, through
// the ONE shared predicate (`isFormattableOperandType`) and the ONE shared
// message builder (`formatVocabularyRejectMessage`) both spellings read. The
// §8.1 arity message is SUPPRESSED for the one call shape the vocabulary gate
// owns (`ToString` with exactly one argument), so the reader still gets
// exactly ONE diagnostic for the one mistake.
//
// THE ORACLE. A rejection is pinned by a diagnostic at a REAL location
// (`failedAtCompileTimeWithRealLocation`) plus the diagnostic's TEXT — never by
// an exit code, and never by the absence of a diagnostic on a program that
// compiles (a successful compile's stderr is not in `r.diagnostics`, so such
// an assertion would be vacuous). The compiling rows pin the program's exact
// stdout instead.
//
// Every row is on `runAot` — a compile-time rejection needs no sanitizer, and
// the `checked_calls` ratchet (`scripts/check_toolchain_skip_surface.py`) must
// not grow.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — the AOT oracle
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

// THE ONE MESSAGE (fix #7377): the sentence both spellings must answer with,
// with the enum's own name in it. §2.18 is the stdlib vocabulary table and
// §18.5 is the spec's dispatch rule — the two halves of the one obligation.
const char* kEnumFormatReject =
    "a format spec requires an intrinsically-formattable operand "
    "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
    "`E` is neither (§2.18; §18.5)";

// The §8.1 arity message — the ONE the member spelling used to answer with.
// The fix removes it for this shape, so it must not appear any more.
const char* kArityMessage =
    "the universal `Object.ToString` member takes 0 argument(s)";

// A program whose `Main` body is `body`, over a one-constant enum `E`.
std::string prog(const std::string& body) {
    return "module M;\n"
           "enum E\n"
           "  A, B;\n"
           "end E;\n"
           "class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The stdout of a program that compiled and ran; empty when it did not run.
std::string runStdout(const std::string& p) {
    JitResult r = runAot(p);
    return r.ran ? r.stdoutText : std::string();
}

// A rejected program: the diagnostic must carry a REAL location (so the
// rejection is a genuine front-end diagnostic, not a crash / signal /
// timeout) and must carry `needle`.
bool rejectedWith(const JitResult& r, const char* needle) {
    return bezel::test::failedAtCompileTimeWithRealLocation(r) &&
           bezel::test::diagnosticMessageContains(r, needle);
}

}  // namespace

// ===========================================================================
// THE DEFECT ROWS — the enum receiver, both spellings, ONE diagnostic.
// ===========================================================================

// The MEMBER spelling. Pre-fix this was §8.1's arity message ("the universal
// `Object.ToString` member takes 0 argument(s) but the call supplies 1
// (§8.1)") — an ARITY explanation for a VOCABULARY fact.
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           the_member_spelling_is_refused_with_the_vocabulary_rule) {
    JitResult r = runAot(
        prog("    var e: E := E.A;\n"
             "    Console.WriteLine(e.ToString(\"F4\"));\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kEnumFormatReject));
}

// The HOLE spelling. Pre-fix this was the §18.5 message — the SAME
// obligation, a DIFFERENT diagnostic, and a reader who wrote the other
// spelling got a different explanation for the same mistake.
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           the_hole_spelling_is_refused_with_the_same_message) {
    JitResult r = runAot(
        prog("    var e: E := E.A;\n"
             "    Console.WriteLine($\"{e:F4}\");\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kEnumFormatReject));
}

// ONE OBLIGATION, ONE DIAGNOSTIC — the member spelling must report the
// vocabulary message EXACTLY ONCE, and the §8.1 arity message NOT AT ALL.
// (The count is over the diagnostic TEXT: a rejected compile packs the
// driver's whole stderr into one Diagnostic entry, so an entry count would
// always read 1 — `diagnosticMessageCount` counts occurrences instead.)
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           the_member_spelling_reports_one_diagnostic_and_no_arity_message) {
    JitResult r = runAot(
        prog("    var e: E := E.A;\n"
             "    Console.WriteLine(e.ToString(\"F4\"));\n"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(
                         r, "a format spec requires an intrinsically-formattable"),
                    1);
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kArityMessage), 0);
}

// The same property on the hole spelling: ONE diagnostic, and no arity
// message (the hole never had one, but the property is what the issue asks
// for, and it is the row that would catch a second report).
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           the_hole_spelling_reports_one_diagnostic) {
    JitResult r = runAot(
        prog("    var e: E := E.A;\n"
             "    Console.WriteLine($\"{e:F4}\");\n"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(
                         r, "a format spec requires an intrinsically-formattable"),
                    1);
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kArityMessage), 0);
}

// The `?.` spelling of the member call is the same obligation: §18.5's
// dispatch rule does not care whether the receiver is lifted. Pre-fix it gave
// the arity message too.
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           the_null_conditional_member_spelling_is_refused_with_the_same_message) {
    JitResult r = runAot(
        prog("    var e: E? := E.A;\n"
             "    Console.WriteLine(e?.ToString(\"F4\"));\n"));
    BEZEL_EXPECT_TRUE(rejectedWith(r, kEnumFormatReject));
    BEZEL_EXPECT_EQ(bezel::test::diagnosticMessageCount(r, kArityMessage), 0);
}

// ===========================================================================
// THE CONTROL ROWS — the shapes the standard library ADMITS must not move.
// ===========================================================================

// The opt-in WORKS: an enum that declares `: IFormattable` AND supplies the
// `ToString(format: string)` that stdlib §2.18's interface declares has a
// format vocabulary of its own, and BOTH spellings reach it (spec §18.5's
// dispatch). This is the row that says the gate asks about the TYPE's
// vocabulary and not about the enum-ness of the receiver.
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           an_enum_declaring_iformattable_admits_both_spellings) {
    const std::string p =
        "module M;\n"
        "enum E: IFormattable\n"
        "  A, B;\n"
        "  public method ToString(format: string): string => \"E:\" + format;\n"
        "end E;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var e: E := E.A;\n"
        "    Console.WriteLine(e.ToString(\"F4\"));\n"
        "    Console.WriteLine($\"{e:F4}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(runStdout(p), "E:F4\nE:F4\n");
}

// The NO-FORMAT `ToString()` is not a format spec and must not be disturbed:
// spec §8.5 renders an enum by its qualified type name ("M.E").
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           an_enum_without_a_format_spec_still_renders_its_qualified_name) {
    BEZEL_EXPECT_EQ(
        runStdout(prog("    var e: E := E.A;\n"
                       "    Console.WriteLine(e.ToString());\n")),
        "M.E\n");
}

// A NUMERIC receiver admits the spec (stdlib §2.18's integer row) — the gate
// must not over-reject a type that HAS a vocabulary.
BEZEL_TEST(enum_format_spec_vocabulary_7377,
           a_numeric_receiver_still_admits_a_format_spec) {
    BEZEL_EXPECT_EQ(
        runStdout(prog("    Console.WriteLine((255).ToString(\"D4\"));\n")),
        "0255\n");
}
