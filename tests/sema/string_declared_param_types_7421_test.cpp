// Spec-derived regression test for issue #7421.
//
// stdlib §9.6 (v0.345) declares the `String` members' PARAMETERS, and
// §5.1 (v0.345) decides whether an argument reaches one of them:
//
//   public pure method Trim(chars: IEnumerable<char>): string;
//   public pure method Equals(other: string): bool;
//   public static pure method IsNullOrWhiteSpace(value: string?): bool;
//   public pure method CompareTo(other: string, ignoreCase: bool): int;
//
// §7.3 (v0.345): "Assigning `null` to a non-nullable target is a compile
// error."
//
// BEFORE THE FIX (measured on 7626bb4d4e), each row = the call, the observed
// behaviour:
//
//   s.Trim(5)                    -> "Call parameter type does not match
//                                    function signature!" + "IR verification
//                                    failed" — the compiler ABORTED;
//   s.Trim('a')                  -> the same abort;
//   s.Trim(null)                 -> SILENTLY ACCEPTED, printed "abc" — §7.3
//                                    forbids it;
//   s.Trim(arrInt)               -> SILENTLY ACCEPTED, printed "abc" — an
//                                    `Array<int>` is not an
//                                    `IEnumerable<char>`;
//   string.IsNullOrWhiteSpace(5) -> "Both operands to ICmp instruction are
//                                    not of the same type!" — the compiler
//                                    ABORTED;
//   s.Equals(5)                  -> the same abort;
//   s.Equals(true)               -> the same abort;
//   s.CompareTo("x", 5)          -> SILENTLY ACCEPTED, printed "-1" — the
//                                    `ignoreCase` parameter is declared
//                                    `bool`.
//
// THE ORACLE. `runAot` — the strict AOT path. A rejection row asserts the
// FULL diagnostic: the DECLARED FORM (stdlib §9.6's own words), the
// PARAMETER NAME, the ARGUMENT'S TYPE and the §-RULE it misses. A §-only
// assertion would be vacuous here, because every refusal of this family says
// §22.6 — so the §5.1 / §7.3 rule is asserted too.
//
// The CONTROL rows pin the value of every legal spelling the fix must keep
// admitting.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program every row runs: the call is the only statement, so a rejected
// call is visibly a no-op when it is (wrongly) accepted.
std::string prog(const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var s := \"abc\";\n"
           "    " + stmt + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& stmt, const std::string& needle) {
    auto r = runAot(prog(stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    " + body + "\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── §9.6 / §5.1: an argument the DECLARED overload set does not name ──────

// stdlib §9.6: `Trim(chars: IEnumerable<char>): string`. §5.1 converts no
// integer type to an `IEnumerable<char>`, so an `int` reaches no declared
// form.
BEZEL_TEST(sema_string_declared_param_types_7421,
           trim_int_chars_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(s.Trim(5));",
        "no matching overload of `string.Trim` — stdlib §9.6 declares "
        "`Trim(chars: IEnumerable<char>): string`, so the `chars` argument "
        "must be an `IEnumerable<char>` — the argument has type `int`, which "
        "is never implicitly convertible to it (§5.1)");
}

// A `char` is not an `IEnumerable<char>` either — §5.1's table has no row
// from `char` to an interface, and §9.6's declared form is the interface.
BEZEL_TEST(sema_string_declared_param_types_7421,
           trim_char_chars_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(s.Trim('a'));",
        "no matching overload of `string.Trim` — stdlib §9.6 declares "
        "`Trim(chars: IEnumerable<char>): string`, so the `chars` argument "
        "must be an `IEnumerable<char>` — the argument has type `char`, which "
        "is never implicitly convertible to it (§5.1)");
}

// An `Array<int>` is not an `IEnumerable<char>`: §4.4 makes an `Array<T>` an
// `IEnumerable<T>` — with the element type it names, not a different one.
BEZEL_TEST(sema_string_declared_param_types_7421,
           trim_int_array_chars_argument_is_rejected) {
    expectRejected(
        "var a: Array<int> := [1]; Console.WriteLine(s.Trim(a));",
        "no matching overload of `string.Trim` — stdlib §9.6 declares "
        "`Trim(chars: IEnumerable<char>): string`, so the `chars` argument "
        "must be an `IEnumerable<char>`");
}

// stdlib §9.6: `Equals(other: string): bool`. The `Object?` slot's spelling
// is reached through the slot (§8.3), so an `int` is not one of the
// declared argument types.
BEZEL_TEST(sema_string_declared_param_types_7421,
           equals_int_other_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(s.Equals(5));",
        "no matching overload of `string.Equals` — stdlib §9.6 declares "
        "`Equals(other: string): bool`, so the `other` argument must be a "
        "`string` or an `Object` — the argument has type `int`, which is "
        "never implicitly convertible to it (§5.1)");
}

// §5.1 lists "between integers and `bool`" as never implicit.
BEZEL_TEST(sema_string_declared_param_types_7421,
           equals_bool_other_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(s.Equals(true));",
        "no matching overload of `string.Equals` — stdlib §9.6 declares "
        "`Equals(other: string): bool`, so the `other` argument must be a "
        "`string` or an `Object` — the argument has type `bool`, which is "
        "never implicitly convertible to it (§5.1)");
}

// stdlib §9.6: `IsNullOrWhiteSpace(value: string?): bool` — the STATIC
// spelling.
BEZEL_TEST(sema_string_declared_param_types_7421,
           isnullorwhitespace_int_value_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(string.IsNullOrWhiteSpace(5));",
        "no matching overload of `string.IsNullOrWhiteSpace` — stdlib §9.6 "
        "declares `IsNullOrWhiteSpace(value: string?): bool`, so the `value` "
        "argument must be a `string` — the argument has type `int`, which is "
        "never implicitly convertible to it (§5.1)");
}

// stdlib §9.6: `CompareTo(other: string, ignoreCase: bool): int` — the
// SECOND parameter is declared `bool`.
BEZEL_TEST(sema_string_declared_param_types_7421,
           compareto_int_ignore_case_argument_is_rejected) {
    expectRejected(
        "Console.WriteLine(s.CompareTo(\"x\", 5));",
        "no matching overload of `string.CompareTo` — stdlib §9.6 declares "
        "`CompareTo(other: string, ignoreCase: bool): int`, so the "
        "`ignoreCase` argument must be a `bool` — the argument has type "
        "`int`, which is never implicitly convertible to it (§5.1)");
}

// ── §7.3: the `null` literal at a NON-nullable parameter ──────────────────

// stdlib §9.6 declares `Trim(chars: IEnumerable<char>): string` — the
// parameter is not `T?`, so §7.3 ("Assigning `null` to a non-nullable
// target is a compile error") applies. The §-RULE is §7.3's, NOT §22.6's.
BEZEL_TEST(sema_string_declared_param_types_7421,
           trim_null_chars_argument_is_rejected_by_section_7_3) {
    expectRejected(
        "Console.WriteLine(s.Trim(null));",
        "the argument to `string.Trim` is nullable but the target type is "
        "non-nullable (§7.3) — stdlib §9.6 declares "
        "`Trim(chars: IEnumerable<char>): string`, whose parameter is not "
        "nullable");
}

// ── CONTROLS: the declared forms still work, with their values pinned ──────

// `Trim(chars: IEnumerable<char>)` at an `Array<char>` — the compiler's
// lowering reads the set as the standard library's `Array<char>`.
BEZEL_TEST(sema_string_declared_param_types_7421, trim_char_array_control) {
    expectStdout(
        "var a: Array<char> := ['a']; var s := \"abc\"; "
        "Console.WriteLine(s.Trim(a));",
        "bc\n");
}

// stdlib §9.6: `Equals(other: string): bool` — the `Object?` slot's spelling
// admits the null, and the member answers it (spec §8.3: "Through the slot
// an argument that is not a `C`, `null` included, compares unequal").
BEZEL_TEST(sema_string_declared_param_types_7421, equals_null_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Equals(null));",
                 "False\n");
}

// stdlib §9.6: `IsNullOrWhiteSpace(value: string?): bool` — the member is
// total over `string?`.
BEZEL_TEST(sema_string_declared_param_types_7421,
           isnullorwhitespace_null_control) {
    expectStdout("Console.WriteLine(string.IsNullOrWhiteSpace(null));",
                 "True\n");
}

BEZEL_TEST(sema_string_declared_param_types_7421, isnullorwhitespace_string_control) {
    expectStdout("Console.WriteLine(string.IsNullOrWhiteSpace(\"x\"));",
                 "False\n");
}

// `Equals(other: string)` at a `string`.
BEZEL_TEST(sema_string_declared_param_types_7421, equals_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Equals(\"abc\"));",
                 "True\n");
}

// `CompareTo(other: string, ignoreCase: bool)` at a `bool`.
BEZEL_TEST(sema_string_declared_param_types_7421, compareto_bool_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.CompareTo(\"x\", true));",
                 "-1\n");
}

// The char-or-string overload sets keep both of their declared forms.
BEZEL_TEST(sema_string_declared_param_types_7421, contains_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains(\"b\"));",
                 "True\n");
}

BEZEL_TEST(sema_string_declared_param_types_7421, contains_char_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.Contains('b'));",
                 "True\n");
}

BEZEL_TEST(sema_string_declared_param_types_7421, indexof_string_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.IndexOf(\"b\"));",
                 "1\n");
}

BEZEL_TEST(sema_string_declared_param_types_7421, indexof_char_control) {
    expectStdout("var s := \"abc\"; Console.WriteLine(s.IndexOf('b'));",
                 "1\n");
}

BEZEL_TEST(sema_string_declared_param_types_7421, lastindexof_string_control) {
    expectStdout("var s := \"abcabc\"; Console.WriteLine(s.LastIndexOf(\"b\"));",
                 "4\n");
}

// `Split(separator: string)`.
BEZEL_TEST(sema_string_declared_param_types_7421, split_string_control) {
    expectStdout(
        "var s := \"a,b\"; var parts := s.Split(\",\"); "
        "Console.WriteLine(parts.Count);",
        "2\n");
}
