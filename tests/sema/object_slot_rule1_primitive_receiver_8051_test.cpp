// Regression tests for #8051 -- on a PRIMITIVE receiver the §8.1
// universal-member commitment (and §26.5 rule 1 with it) was not applied, so
// (a) a user extension whose name collides with a universal `Object` member
// ANSWERED the call, displacing the committed instance member, and (b) a
// wrong-arity call to one of those members was silently ADMITTED and degraded
// at run time instead of being refused at compile time.
//
// THE SPEC.
//
// Language spec §8.1 (v0.353) declares `Object` and states verbatim: "`Object`
// is the universal root. Every type derives from it: classes ... directly
// through the inheritance chain ...; records, enums, choices, PRIMITIVES,
// inline arrays, and the unit type derive via boxing (§8.6) when used in
// `Object`-typed positions". So a PRIMITIVE receiver carries the four
// universal instance members `ToString`, `Equals`, `GetHashCode` and `GetType`
// exactly as a user class does.
//
// Language spec §26.5 rule 1 (v0.353): "If `x`'s declared type (or any base
// type) *declares* any instance member named `Foo` -- of any arity,
// applicability, or accessibility -- instance resolution is committed and
// extension blocks are **never consulted** for that name on that receiver. If
// the committed instance lookup then fails (no applicable overload, §22.5/
// §22.6, or the member is inaccessible, §13.1), the call is a compile error
// citing the instance member(s); it does not fall back to an extension."
//
// #7824 (picked as `373cc7cf`) closed that rule for the USER-TYPE receivers
// (a class / record / interface / enum): the committed instance member has no
// MemberSymbol (§8.1's universal members are synthesized), so the call reaches
// typeCall's universal arity check and is refused with
//
//     the universal `Object.ToString` member takes 0 argument(s) but the call
//     supplies 1 (§8.1)
//
// THE PRIMITIVE RECEIVER WAS THE HOLE. MEASURED at the parent (`99aea6a2`)
// with `build/bezelc`, on `var x: int := 5;`:
//
//   * `x.Equals(1, 2)` COMPILED (no diagnostic at all) and the enclosing
//     statement emitted NOTHING -- the #2434 degradation, with only an
//     `internal:` warning ("the argument of this `Console.WriteLine` failed to
//     lower to a value, so the whole statement emits nothing");
//   * `x.GetHashCode(1)` and `x.GetType(1)` likewise COMPILED and emitted
//     nothing;
//   * with a user extension of the same name, the EXTENSION answered:
//     `extension int { Equals(a: int, b: int): bool => true; }` printed
//     `True` for `x.Equals(1, 2)`, `extension int { GetHashCode(): int => 7;
//     }` printed `7` for `x.GetHashCode()`, and `extension int { GetType():
//     int => 7; }` printed `7` for `x.GetType()` -- where §8.2 refuses the
//     call outright.
//
// THE FIX refuses the primitive receiver at COMPILE TIME with the SAME
// diagnostic the user-type receiver gets, and stops the extension blocks from
// being consulted for the four universal names on a primitive receiver.
//
// ⚠ THE ISSUE'S LITERAL REPRO IS NOT THIS DEFECT, AND IS NOT REFUSED HERE.
// `x.ToString(3)` on an `int` is stdlib §9.1's OWN declared member
// `ToString(radix: int)`, and radix 3 is not in the admitted set {2, 8, 10,
// 16}, so stdlib §9.1's own contract -- "takes the same radix set, raising
// `ArgumentException` for any other" -- makes it a RUN-TIME fault. That is a
// pinned, tested contract (tests/codegen/radix_validity_7268_test.cpp's
// `unadmitted_radix_at_tostring_raises` and `radix_36_at_tostring_raises`)
// and the conformance corpus agrees ("`expect: run`" case `l2_3_0003` --
// stdlib §2.3, `Convert.ToString(42, 3)` -- CATCHES the exception at RUN
// TIME, with a LITERAL radix). MEASURED at the parent: the extension is NOT
// what answers `x.ToString(3)` -- the program throws the SAME
// `System.ArgumentException` with NO extension in the program at all, and the
// stdlib §9.1 overload answers at the admitted radices either way. So the
// extension was never consulted for that NAME (rule 1 already held for
// `ToString`, whose arity the primitive's own surface owns), and a
// compile-time refusal would break a pinned contract and refuse a legal call
// shape (`x.ToString(16)` is the same overload). The two rows
// `the_issue_repro_*` below pin that boundary explicitly.
//
// ORACLE: the refusal TEXT for the refused rows, and the observed VALUE for
// the rows where the committed member is the answer. No row asserts the
// absence of a diagnostic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A severity-aware scan: the refusal rows assert an ERROR diagnostic, not
// merely that some diagnostic's text matches. The shared
// `bezel::test::diagnosticMessageContains` scans every severity, so this
// local helper adds the severity test -- under a FRESH name, because the
// shared header's own header comment records why a `hasError`-shaped name
// here would collide by ADL with `bezel::test`'s own.
bool errorDiagnosticContains(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The program is REFUSED at compile time with `needle` among its ERROR
// diagnostics, it did NOT run, and the rejection carries a real source
// location (not a 0:0 synthetic outcome -- see
// `bezel::test::failedAtCompileTimeWithRealLocation`'s header).
void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(errorDiagnosticContains(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The program RAN, exited 0, and printed `out`.
void expectOut(const std::string& src, const std::string& out) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The program RAN and the runtime reported an UNCAUGHT `ArgumentException`
// at the no-handler boundary: exit 70, the house's uncaught-exception code
// (spec §33.3), and nothing on stdout (the exception text goes to stderr).
// The `r.ran` assertion comes FIRST and is LOAD-BEARING: it is what makes a
// program that did not run visible as such rather than as a wrong value.
void expectUncaughtArgumentException(const JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 70);
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : std::string("<did not run>"),
                   std::string(""));
}

// A program over an `int` receiver, with `decls` (the extension blocks) in
// front of it.
std::string intProgram(const std::string& decls, const std::string& body) {
    return "module R;\n" + decls +
           "class Program public static method Main(): int\n" + body +
           "  return 0;\n"
           "end Main; end Program;\n";
}

}  // namespace

// ---- the defect rows --------------------------------------------------------

// §26.5 rule 1 + §8.1: `Object` declares `Equals(other: Object?)`, and the
// `int` receiver's own surface declares `Equals(other: int)` (stdlib §9.1:
// "Equals(other: int): bool — auto-generated from IEquatable<int> ...; not
// shown explicitly") -- arity 1 either way. A two-argument extension of that
// name is unreachable, and the committed instance lookup finds no applicable
// overload, so the call is a compile error (NOT the extension's value).
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_equals_two_arg_extension_does_not_bind) {
    expectRejected(
        intProgram("extension int public method Equals(a: int, b: int): bool "
                   "=> true; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.Equals(1, 2));\n"),
        "the universal `Object.Equals` member takes 1 argument(s) but the call "
        "supplies 2 (§8.1)");
}

// The same call with NO extension in the program: the commitment is the
// RECEIVER's, not the extension's, so the refusal is the same. Before the
// fix this compiled with no diagnostic and the statement emitted nothing.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_equals_two_args_without_an_extension_is_refused) {
    expectRejected(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.Equals(1, 2));\n"),
        "the universal `Object.Equals` member takes 1 argument(s) but the call "
        "supplies 2 (§8.1)");
}

// §8.1's `GetHashCode()` takes no argument. An extension of that name is
// unreachable on an `int` receiver, and the committed lookup finds no
// applicable overload for a one-argument call.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_gethashcode_with_an_argument_is_refused) {
    expectRejected(
        intProgram("extension int public method GetHashCode(n: int): int => "
                   "7; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetHashCode(1));\n"),
        "the universal `Object.GetHashCode` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The same, with no extension in the program.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_gethashcode_with_an_argument_without_an_extension_is_refused) {
    expectRejected(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetHashCode(1));\n"),
        "the universal `Object.GetHashCode` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// §8.1's `GetType()` takes no argument, and §8.2 refuses the call outright
// ("reflection is not yet implemented"). An extension of that name must not
// answer it either.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_gettype_with_an_argument_is_refused) {
    expectRejected(
        intProgram("extension int public method GetType(n: int): int => 7; "
                   "end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetType(1));\n"),
        "the universal `Object.GetType` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The same, with no extension in the program.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           int_gettype_with_an_argument_without_an_extension_is_refused) {
    expectRejected(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetType(1));\n"),
        "the universal `Object.GetType` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// The `?.` spelling is the same call and gets the same refusal (the `?.`
// receiver path is a separate arm).
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           null_conditional_int_equals_two_args_is_refused) {
    expectRejected(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x?.Equals(1, 2));\n"),
        "the universal `Object.Equals` member takes 1 argument(s) but the call "
        "supplies 2 (§8.1)");
}

// The `?.` twin of the `GetHashCode` row.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           null_conditional_int_gethashcode_with_an_argument_is_refused) {
    expectRejected(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x?.GetHashCode(1));\n"),
        "the universal `Object.GetHashCode` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// ---- the control rows -------------------------------------------------------

// §8.1's universal members keep answering on a PRIMITIVE receiver, and a
// same-named user extension does not displace them: `ToString()` and
// `Equals(one)` are the committed members' answers, not the extension's.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_universal_members_still_answer_on_a_primitive) {
    expectOut(
        intProgram("extension int public method ToString(): string => \"EXT\"; "
                   "end;\n"
                   "extension int public method Equals(other: int): bool => "
                   "true; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString());\n"
                   "  Console.WriteLine(x.Equals(3));\n"),
        "5\nFalse\n");
}

// The same three calls with no extension in the program: identical answers.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_universal_members_still_answer_without_an_extension) {
    expectOut(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString());\n"
                   "  Console.WriteLine(x.Equals(3));\n"),
        "5\nFalse\n");
}

// A same-named extension does not displace the universal `GetHashCode()`:
// the answer is the identity hash, not the extension's sentinel. The
// identity hash is drawn from a per-domain sequence (§8.3), so the row
// compares it against the sentinel rather than against a literal.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           a_same_named_extension_does_not_displace_gethashcode) {
    expectOut(
        intProgram("extension int public method GetHashCode(): int => "
                   "999999; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetHashCode() <> 999999);\n"),
        "True\n");
}

// A same-named extension does not displace `GetType()` either: §8.2 refuses
// the call ("reflection is not yet implemented"), which is what the committed
// member's own answer is on this compiler.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           a_same_named_extension_does_not_displace_gettype) {
    expectRejected(
        intProgram("extension int public method GetType(): int => 999999; "
                   "end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.GetType());\n"),
        "reflection is not yet implemented: 'GetType()'");
}

// stdlib §9.1's OWN `ToString(radix: int)` and `ToString(format: string)`
// overloads keep answering on an `int` receiver -- they are the primitive's
// own declared surface, so §26.5 rule 1 commits the NAME to them and the
// §8.1 universal arity (0) does not apply. This row is what makes the
// issue's literal repro (`x.ToString(3)`) a documented RUN-TIME fault rather
// than a compile-time one: radix 3 is not in the admitted set {2, 8, 10, 16},
// and stdlib §9.1 says the call "raises `ArgumentException` for any other".
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           tostring_radix_overload_still_answers) {
    expectOut(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString(16));\n"
                   "  Console.WriteLine(x.ToString(2));\n"),
        "5\n101\n");
}

// The USER-TYPE receiver keeps the refusal it has always had -- the same
// sentence the primitive receiver now gets (this is #7824's row, repeated
// here so the two receivers' answers can be compared side by side).
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_user_class_receiver_still_refuses) {
    expectRejected(
        "module R;\n"
        "public class C public constructor(X: int) end constructor; end C;\n"
        "extension C public method ToString(n: int): string => \"EXT\"; end;\n"
        "class Program public static method Main(): int\n"
        "  var x: C := new C(X := 5);\n"
        "  Console.WriteLine(x.ToString(3));\n"
        "  return 0;\n"
        "end Main; end Program;\n",
        "the universal `Object.ToString` member takes 0 argument(s) but the "
        "call supplies 1 (§8.1)");
}

// A same-named extension does not displace stdlib §9.1's `ToString` either:
// the primitive's OWN surface answers, at the arities §9.1 declares. This is
// the positive half of the rule-1 commitment for `ToString` -- the NAME is
// committed to the primitive's declared overloads, so the extension blocks
// are never consulted for it.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           a_same_named_extension_does_not_displace_tostring) {
    expectOut(
        intProgram("extension int public method ToString(n: int): string => "
                   "\"EXT\"; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString(16));\n"),
        "5\n");
}

// The ISSUE'S LITERAL REPRO, with the extension in the program: the program
// COMPILES and the runtime reports an uncaught `ArgumentException` (exit 70).
// The exception is stdlib §9.1's radix contract, not the extension's doing --
// the row below runs the SAME program with NO extension and gets the SAME
// answer, and `a_same_named_extension_does_not_displace_tostring` shows the
// stdlib §9.1 overload answering at an admitted radix either way.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_issue_repro_with_the_extension_raises_at_run_time) {
    auto r = runAot(
        intProgram("extension int public method ToString(n: int): string => "
                   "\"EXT\" + n.ToString(); end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString(3));\n"));
    expectUncaughtArgumentException(r);
}

// The SAME program with NO extension at all: the same uncaught
// `ArgumentException`. The extension is therefore NOT what answers the
// issue's repro -- the primitive's own `ToString(radix: int)` is, and radix 3
// is outside stdlib §9.1's admitted set {2, 8, 10, 16}.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_issue_repro_without_the_extension_raises_at_run_time) {
    auto r = runAot(
        intProgram("",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.ToString(3));\n"));
    expectUncaughtArgumentException(r);
}

// §26.5 rule 2 still holds on a primitive receiver: a name the primitive's
// declared surface does NOT declare is still answered by the extension. The
// rule-1 commitment above is about the four universal NAMES only.
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           a_name_the_primitive_does_not_declare_still_binds) {
    expectOut(
        intProgram("extension int public method Tag(): int => 42; end;\n",
                   "  var x: int := 5;\n"
                   "  Console.WriteLine(x.Tag());\n"),
        "42\n");
}

// The `string` head was never this hole: stdlib §9.6 declares the string's
// OWN `Equals(other: string)` surface, so the NAME is committed to it and a
// same-named extension does not answer. (The string is a reference type with
// its own §9.6 gates -- see the note in
// `primitiveReceiverCommitsObjectSlotMember`'s header.)
BEZEL_TEST(object_slot_rule1_primitive_receiver_8051,
           the_string_head_commits_its_own_surface) {
    expectOut(
        "module R;\n"
        "extension string public method Equals(other: string): bool => true; "
        "end;\n"
        "class Program public static method Main(): int\n"
        "  var s: string := \"abc\";\n"
        "  Console.WriteLine(s.Equals(\"xyz\"));\n"
        "  return 0;\n"
        "end Main; end Program;\n",
        "False\n");
}
