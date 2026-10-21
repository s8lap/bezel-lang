// Regression tests for #6562 — `string.IndexOf`'s search value at position 0.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.339) declares exactly two `string` overloads whose first
//     parameter is the search value: `IndexOf(value: char, startIndex: int :=
//     0)` and `IndexOf(value: string, startIndex: int := 0)`. No overload takes
//     an `int` at position 0.
//   * spec §5.1 (v0.339): `int` never converts implicitly to `char`, and no
//     integer type converts to `string`. An `int` search value therefore has no
//     conforming overload, whether it is plain, narrowed or un-narrowed `int?`.
//   * spec §7.4 (v0.339): narrowing removes only the null case. A narrowed
//     `int?` is still an `int`, so it has no conforming overload either.
//
// PRE-FIX, sema admitted every such argument: `paramKind` returned None for a
// non-char, non-string value at position 0, so the argument was never checked.
// Codegen then failed on each of them with the same LLVM verifier ICE ("Call
// parameter type does not match function signature!"). Each negative row
// asserts the dedicated diagnostic text, and the un-narrowed row also asserts
// that the §7 "nullable" wording is ABSENT, because that wording would point
// the reader at the wrong remedy: no narrowing makes an `int` a `char` or a
// `string`.
//
// The positive controls guard against over-rejection. Position 0 still admits
// `char` and `string`, and position 1 (`startIndex`) still admits `int`.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// True iff any reported diagnostic is an Error.
bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

// The dedicated first-position diagnostic (fix #6562).
constexpr const char* kFirstPositionDiag =
    "takes its search value at position 0 as a `char` or a `string`";

// The §7 wording a generic assignability message would produce for a nullable
// argument. It must NOT appear for the position-0 rejection.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

// Wraps `body` (statements) in a class whose `Main` returns 0.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// §9.6 / §5.1: a plain `int` has no conforming search-value overload.
BEZEL_TEST(sema_indexof_int_arg_6562, plain_int_search_value_rejected) {
    auto r = parseSemaAll(prog(
        "    var n: int := 1;\n"
        "    Console.WriteLine(\"abc\".IndexOf(n).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kFirstPositionDiag));
}

// The issue's repro. §7.4 narrows `n` to non-null, but it is still an `int`
// (§7.4 removes only the null case), so the call is rejected.
BEZEL_TEST(sema_indexof_int_arg_6562, narrowed_int_q_search_value_rejected) {
    auto r = parseSemaAll(prog(
        "    var n: int? := 1;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(n).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kFirstPositionDiag));
}

// An un-narrowed `int?` (`:= null`: a non-null declaration initializer narrows,
// §7.4 v0.339, #6223) gets the same dedicated message and NOT the §7 wording.
// Narrowing could not fix this call, so a §7 "nullable" message would name the
// wrong remedy.
BEZEL_TEST(sema_indexof_int_arg_6562, unnarrowed_int_q_search_value_rejected) {
    auto r = parseSemaAll(prog(
        "    var n: int? := null;\n"
        "    Console.WriteLine(\"abc\".IndexOf(n).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kFirstPositionDiag));
    BEZEL_EXPECT_FALSE(hasError(r, kNullableDiag));
}

// The two-argument overload has the same position-0 search value.
BEZEL_TEST(sema_indexof_int_arg_6562,
           narrowed_int_q_search_value_two_arg_rejected) {
    auto r = parseSemaAll(prog(
        "    var n: int? := 1;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf(n, 0).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kFirstPositionDiag));
}

// The rule is type-wide, not an int special case: `bool` has no §5.1
// conversion to `char` or `string` either.
BEZEL_TEST(sema_indexof_int_arg_6562, bool_search_value_rejected) {
    auto r = parseSemaAll(prog(
        "    Console.WriteLine(\"abc\".IndexOf(true).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kFirstPositionDiag));
}

// Control (§9.6): `char` and `string` search values still type-check at
// position 0, with and without the startIndex argument.
BEZEL_TEST(sema_indexof_int_arg_6562,
           char_and_string_search_values_still_accepted) {
    auto r = parseSemaAll(prog(
        "    Console.WriteLine(\"abc\".IndexOf('b').ToString());\n"
        "    Console.WriteLine(\"abc\".IndexOf(\"b\").ToString());\n"
        "    Console.WriteLine(\"abc\".IndexOf('b', 0).ToString());\n"
        "    Console.WriteLine(\"abc\".IndexOf(\"b\", 0).ToString());\n"));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// Control (§9.6, startIndex: int): a plain `int` at position 1 is admitted.
BEZEL_TEST(sema_indexof_int_arg_6562, plain_int_start_index_still_accepted) {
    auto r = parseSemaAll(prog(
        "    var n: int := 1;\n"
        "    Console.WriteLine(\"abc\".IndexOf('b', n).ToString());\n"
        "    Console.WriteLine(\"abc\".IndexOf(\"b\", n).ToString());\n"));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// Control (§7.4): a narrowed `int?` startIndex is admitted by sema. Its value
// is pinned by the codegen track.
BEZEL_TEST(sema_indexof_int_arg_6562,
           narrowed_int_q_start_index_still_accepted) {
    auto r = parseSemaAll(prog(
        "    var n: int? := 1;\n"
        "    if n is null then return 1; end;\n"
        "    Console.WriteLine(\"abc\".IndexOf('b', n).ToString());\n"));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §7 gate on position 1 (unchanged by #6562): an un-narrowed `int?` startIndex
// is rejected with the §7 wording, and NOT with the position-0 message.
BEZEL_TEST(sema_indexof_int_arg_6562,
           unnarrowed_int_q_start_index_still_rejected_spec_7) {
    auto r = parseSemaAll(prog(
        "    var n: int? := null;\n"
        "    Console.WriteLine(\"abc\".IndexOf('b', n).ToString());\n"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_FALSE(hasError(r, kFirstPositionDiag));
}

// Control (§7.4, v0.339): a declaration `var n: int? := 1;` is the
// declaration's assignment, so it narrows `n` non-null exactly as `n := 1;`
// does (#6223). The startIndex read at position 1 is therefore admitted; the
// `:= null` row above is the genuinely un-narrowed form (#6693).
BEZEL_TEST(sema_indexof_int_arg_6562,
           declaration_narrowed_int_q_start_index_accepted_spec_7_4) {
    auto r = parseSemaAll(prog(
        "    var n: int? := 1;\n"
        "    Console.WriteLine(\"abc\".IndexOf('b', n).ToString());\n"));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
