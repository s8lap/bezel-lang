// Spec-derived regression test for issue #7494.
//
// spec §3.4 (v0.355, line 504) "Keyword aliases for built-in types":
//   "Static members of the standard library type are accessible through either
//    form: `int.Parse`, `int.MaxValue`, `double.IsNaN`, `string.IsNullOrWhiteSpace`."
// spec §1.4 (v0.355, line 78):
//   "The lowercase keyword `string` and the standard-library type name `String`
//    (`System.String`) name the same type — they are interchangeable in source".
// spec §5.1 (v0.355) "Implicit conversions";
// spec §2.3 (v0.355, line 152) "Always-available names" — the first row
//   ("Primitive value types, their reserved PascalCase aliases, and
//   `string`/`String`") may NOT be shadowed;
// stdlib §9.6 (v0.355) `String`.
//
// THE DEFECT. The §9.6 STATIC head has TWO spellings — the primitive keyword
// `string` and its §3.4 PascalCase alias `String` — and the §9.6
// declared-parameter gate (`ExprTC::checkBuiltinStringDeclaredParams`) is the
// gate that asks the §5.1 question of the DECLARED parameter types. It read
// the head's EXPRESSION TYPE first and reached the name-based spellings only
// in an `else if`, so a head that carried a VALUE type stood the whole gate
// down — while the arity gate (which keys on `tid->name == "string" ||
// tid->name == "String"`), the primitive-static member resolver (which keys
// on `primitiveFromName(id2->name)`) and codegen all still read the head as
// the §9.6 static:
//
//   var String := 5;
//   String.IsNullOrWhiteSpace(5)  -> the COMPILER ABORTED ("Both operands to
//                                    ICmp instruction are not of the same type!")
//   string.IsNullOrWhiteSpace(5)  -> REFUSED (§5.1)
//   var String := 5;
//   String.Repeat(5, 2)            -> compiled CLEAN (silently admitted)
//   string.Repeat(5, 2)            -> REFUSED (§5.1)
//
// ⇒ §3.4 says the two spellings are INTERCHANGEABLE; they were not.
//
// The rows below pin the §3.4 rule directly: every call shape is run in BOTH
// spellings and the two are required to produce the SAME diagnostic text
// (or the SAME stdout), so a future fix cannot cover one spelling and leave
// the other.
//
// THE ORACLE is the exact diagnostic text (plus a real source location, which
// a compiler CRASH cannot fake) for the refusal rows and the exact stdout for
// the accept rows — never the absence of a diagnostic.
#include <regex>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The text of the first error diagnostic, with the driver's
// `<path>:<line>:<col>: ` prefix stripped. The path is a per-run temporary
// file, so it differs between the two runs of the SAME program and cannot be
// part of an "the two spellings say the same thing" comparison.
std::string errorText(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        static const std::regex kLoc(R"([^\s:]*:\d+:\d+: )");
        return std::regex_replace(d.message, kLoc, "");
    }
    return "";
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The same program with a LOCAL named `String` in scope. §2.3's first row
// says the name may not be shadowed, but the compiler accepts this local —
// and the §9.6 STATIC head is read BY NAME by the rest of the compiler, so
// the local does not change what `String` names in the head position.
std::string progWithLocal(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var String := 5;\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// `{H}` is the §3.4 head spelling under test.
std::string withHead(const std::string& tmpl, const std::string& head) {
    std::string out;
    for (std::size_t i = 0; i < tmpl.size();) {
        if (tmpl.compare(i, 3, "{H}") == 0) {
            out += head;
            i += 3;
        } else {
            out += tmpl[i];
            ++i;
        }
    }
    return out;
}

// §3.4: the two spellings name the same type, so they are interchangeable —
// the SAME call shape must be refused with the SAME words, and a refusal is
// a REFUSAL (a real source location), never a compiler crash.
void expectBothSpellingsRefused(const std::string& tmpl,
                                 const std::string& needle) {
    auto alias = runAot(prog(withHead(tmpl, "String")));
    auto keyword = runAot(prog(withHead(tmpl, "string")));
    BEZEL_EXPECT_FALSE(alias.ran);
    BEZEL_EXPECT_FALSE(keyword.ran);
    BEZEL_EXPECT_TRUE(hasError(alias, needle));
    BEZEL_EXPECT_TRUE(hasError(keyword, needle));
    BEZEL_EXPECT_EQ(errorText(alias), errorText(keyword));
    // A compiler CRASH is reported as a synthetic diagnostic with no source
    // location, so these two are what separate a refusal from an abort.
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(alias));
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(keyword));
}

// The same rule in the shape that survived the #8046 fix: a LOCAL named
// `String` in scope. The head is still read by NAME (§3.4), so the two
// spellings must still agree — and the call must be refused, not admitted
// and not an abort.
void expectBothSpellingsRefusedUnderLocal(const std::string& tmpl,
                                           const std::string& needle) {
    auto alias = runAot(progWithLocal(withHead(tmpl, "String")));
    auto keyword = runAot(progWithLocal(withHead(tmpl, "string")));
    BEZEL_EXPECT_FALSE(alias.ran);
    BEZEL_EXPECT_FALSE(keyword.ran);
    BEZEL_EXPECT_TRUE(hasError(alias, needle));
    BEZEL_EXPECT_TRUE(hasError(keyword, needle));
    BEZEL_EXPECT_EQ(errorText(alias), errorText(keyword));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(alias));
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(keyword));
}

// The GOOD call still works through BOTH spellings, with the same output.
void expectBothSpellingsRun(const std::string& tmpl, const std::string& out) {
    auto alias = runAot(prog(withHead(tmpl, "String")));
    auto keyword = runAot(prog(withHead(tmpl, "string")));
    BEZEL_EXPECT_TRUE(alias.ran);
    BEZEL_EXPECT_TRUE(keyword.ran);
    BEZEL_EXPECT_EQ(alias.stdoutText, out);
    BEZEL_EXPECT_EQ(keyword.stdoutText, out);
}

// The §5.1 refusal of the §9.6 declared-parameter gate: the gate names the
// DECLARED FORMS, the PARAMETER (§22.5's declared name), the DECLARED TYPE
// at that position and the argument's own type.
std::string mustBe(const std::string& member, const std::string& decl,
                   const std::string& param, const std::string& mustBePhrase,
                   const std::string& argType) {
    return "no matching overload of `string." + member + "` — stdlib §9.6 "
           "declares `" + decl + "`, so the `" + param +
           "` argument must be " + mustBePhrase +
           " — the argument has type `" + argType +
           "`, which is never implicitly convertible to it (§5.1)";
}

}  // namespace

// ───────────────────────── the §9.6 STATIC head, no local in scope ─────────
// The issue's own repro. The §5.1 hole: an `int` at stdlib §9.6's
// `IsNullOrWhiteSpace(value: string?)` parameter.
BEZEL_TEST(string_alias_static_head_7494,
           is_null_or_white_space_with_an_int_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.IsNullOrWhiteSpace(5));",
        mustBe("IsNullOrWhiteSpace", "IsNullOrWhiteSpace(value: string?): bool",
               "value", "a `string`", "int"));
}

// The issue's second half: `Repeat` declares `Repeat(c: char, count: int)` and
// `Repeat(unit: string, count: int)`, and an `int` is neither (§5.1).
BEZEL_TEST(string_alias_static_head_7494,
           repeat_with_an_int_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.Repeat(5, 2));",
        mustBe("Repeat",
               "Repeat(c: char, count: int): string` / "
               "`Repeat(unit: string, count: int): string",
               "c", "a `string` or a `char`", "int"));
}

// `From(buffer: Array<char>, start: int, length: int)` — an `int` at the
// `buffer` parameter (§5.1).
BEZEL_TEST(string_alias_static_head_7494,
           from_with_an_int_buffer_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.From(5, 6, 7));",
        mustBe("From",
               "From(buffer: Array<char>, start: int, length: int): string",
               "buffer", "an `Array<char>`", "int"));
}

// `FromCStringPointer(p: IntPtr)` — an `int` at the `p` parameter.
BEZEL_TEST(string_alias_static_head_7494,
           from_cstring_pointer_with_an_int_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.FromCStringPointer(5));",
        "no matching overload of `String.FromCStringPointer`");
}

// `Join(separator: string, items: IEnumerable<string>)` — an `int` at the
// `separator` parameter.
BEZEL_TEST(string_alias_static_head_7494,
           join_with_an_int_separator_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.Join(5, 2));",
        "the separator argument to `string.Join` has a type that is not "
        "implicitly convertible to the target type (§5.1)");
}

// `IsNullOrEmpty` is NOT a stdlib §9.6 member any more — it was retired at
// v0.290 (fix #3854) — so this row is the RETIRED-MEMBER refusal, not the
// §5.1 one. It is pinned because it travels the same §9.6 head, and the two
// spellings must say the same thing about it too.
BEZEL_TEST(string_alias_static_head_7494,
           the_retired_is_null_or_empty_is_refused_in_both_spellings) {
    expectBothSpellingsRefused(
        "Console.WriteLine({H}.IsNullOrEmpty(5));",
        "'String.IsNullOrEmpty' was REMOVED from the standard library at "
        "v0.290 (stdlib §9.6)");
}

// ─────────────────── the §9.6 STATIC head, a LOCAL named `String` ─────────
// §2.3's first row says the name may not be shadowed, but the compiler
// accepts the local — and the §9.6 head is read BY NAME, so the local does
// not change what the head names. These rows are the shape in which the two
// spellings still diverged: the ALIAS spelling ABORTED the compiler and
// admitted `Repeat` silently, while the KEYWORD spelling refused both.
BEZEL_TEST(string_alias_static_head_7494,
           is_null_or_white_space_with_an_int_is_refused_in_both_spellings_under_a_local_named_string) {
    expectBothSpellingsRefusedUnderLocal(
        "Console.WriteLine({H}.IsNullOrWhiteSpace(5));",
        mustBe("IsNullOrWhiteSpace", "IsNullOrWhiteSpace(value: string?): bool",
               "value", "a `string`", "int"));
}

BEZEL_TEST(string_alias_static_head_7494,
           repeat_with_an_int_is_refused_in_both_spellings_under_a_local_named_string) {
    expectBothSpellingsRefusedUnderLocal(
        "Console.WriteLine({H}.Repeat(5, 2));",
        mustBe("Repeat",
               "Repeat(c: char, count: int): string` / "
               "`Repeat(unit: string, count: int): string",
               "c", "a `string` or a `char`", "int"));
}

BEZEL_TEST(string_alias_static_head_7494,
           from_with_an_int_buffer_is_refused_in_both_spellings_under_a_local_named_string) {
    expectBothSpellingsRefusedUnderLocal(
        "Console.WriteLine({H}.From(5, 6, 7));",
        mustBe("From",
               "From(buffer: Array<char>, start: int, length: int): string",
               "buffer", "an `Array<char>`", "int"));
}

BEZEL_TEST(string_alias_static_head_7494,
           join_with_an_int_separator_is_refused_in_both_spellings_under_a_local_named_string) {
    expectBothSpellingsRefusedUnderLocal(
        "Console.WriteLine({H}.Join(5, 2));",
        "the separator argument to `string.Join` has a type that is not "
        "implicitly convertible to the target type (§5.1)");
}

// ─────────────────────────────── the accepting calls ───────────────────────
// The GOOD call still works through the ALIAS, and through the keyword, with
// the same output.
BEZEL_TEST(string_alias_static_head_7494,
           is_null_or_white_space_runs_in_both_spellings) {
    expectBothSpellingsRun(
        "Console.WriteLine({H}.IsNullOrWhiteSpace(\"  \"));", "True\n");
}

BEZEL_TEST(string_alias_static_head_7494,
           is_null_or_white_space_of_a_non_blank_string_runs_in_both_spellings) {
    expectBothSpellingsRun(
        "Console.WriteLine({H}.IsNullOrWhiteSpace(\"x\"));", "False\n");
}

BEZEL_TEST(string_alias_static_head_7494,
           repeat_with_a_char_runs_in_both_spellings) {
    expectBothSpellingsRun("Console.WriteLine({H}.Repeat('a', 3));", "aaa\n");
}

BEZEL_TEST(string_alias_static_head_7494,
           repeat_with_a_string_runs_in_both_spellings) {
    expectBothSpellingsRun("Console.WriteLine({H}.Repeat(\"ab\", 2));",
                            "abab\n");
}
