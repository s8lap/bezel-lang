// Spec-derived regression test for issue #7448 — a USER's extension over
// `Array<T>` was SILENTLY DROPPED: the collection built-in arm claimed the
// operator name by NAME, so the program ran and printed the BUILT-IN's
// value.
//
// THE SPEC.
//
//   spec §26.5 "Extension members" (v0.350) rule 2: "If the receiver's
//   type declares no instance member of that name, the compiler searches
//   the current module and all imported modules ... for extension blocks on
//   `x`'s type, base types, or implemented interfaces".
//
//   spec §26.5 rule 3: "Most specific type wins.  Extensions on the most
//   specific type are preferred per the subtype ordering below."
//
//   `Array<T>` declares NO instance member named `Sum` (stdlib §4.4 declares
//   `Count` / `Capacity`), and the stdlib's `Sum` is an EXTENSION over
//   `IEnumerable<T>` (stdlib §5).  So a user's `extension Array<int> Sum()`
//   is a MORE SPECIFIC extension than the stdlib's over `IEnumerable<T>`,
//   and §26.5 rule 3 says the USER's wins.
//
// THE DEFECT.
//
// `Emitter::tryLowerCollectionInstanceCall` claimed the stdlib §5 operators
// that only `extension IEnumerable<T>` supplies (Sum / Min / Max / First /
// ...) BY NAME, so the user's extension — which sema had already bound as
// the most specific candidate under §26.5 rules 2 and 3 — was dropped in
// codegen and the built-in answered instead.  The program compiled with NO
// diagnostic, ran, and printed the WRONG value: a silent-acceptance
// inversion.
//
// THE ORACLE IS POSITIVE: every row asserts the VALUE the extension
// returns.  A program that silently dropped the extension prints the
// built-in's value, so the row fails.
//
// THE CONTROLS pin the other side of §26.5 and of §26.5 rule 1: with no
// user extension the built-in answers, and a name the receiver's own
// DECLARED surface carries is answered by the instance member — §26.5
// rule 1 commits to instance resolution for such a name, so an extension
// block is never consulted for it.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module R; " + ext +
           " class Program public static method Main(): int "
           "  var a: Array<int> := [1,2,3]; " + body +
           "  return 0; "
           " end Main; end Program;";
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — §26.5 rules 2 and 3, stdlib §5.
// ===========================================================================
BEZEL_TEST(user_extension_over_builtin_operator_7448,
           array_sum_user_extension_wins_over_the_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Sum(): int => 77; end;",
        "  Console.WriteLine(a.Sum().ToString());")),
        std::string("77"));
}

// ===========================================================================
// THE SAME, FOR THE OTHER §5-ONLY OPERATOR NAMES.
//
// The remedy the issue asks for is the whole operator family, not one name:
// the built-in arm must claim a §5-only name only when the receiver's
// declared surface does not declare it.  These four rows are that family.
// ===========================================================================
BEZEL_TEST(user_extension_over_builtin_operator_7448,
           array_min_user_extension_wins_over_the_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Min(): int => 77; end;",
        "  Console.WriteLine(a.Min().ToString());")),
        std::string("77"));
}

BEZEL_TEST(user_extension_over_builtin_operator_7448,
           array_average_user_extension_wins_over_the_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Average(): int => 77; end;",
        "  Console.WriteLine(a.Average().ToString());")),
        std::string("77"));
}

BEZEL_TEST(user_extension_over_builtin_operator_7448,
           array_first_user_extension_wins_over_the_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method First(): int => 77; end;",
        "  Console.WriteLine(a.First().ToString());")),
        std::string("77"));
}

// ===========================================================================
// THE CONTROL — with NO user extension the built-in answers (stdlib §5's
// `Sum` over `IEnumerable<T>` sums the elements).
// ===========================================================================
BEZEL_TEST(user_extension_over_builtin_operator_7448,
           without_a_user_extension_the_builtin_sum_answers) {
    BEZEL_EXPECT_EQ(runAotOut(prog("", "  Console.WriteLine(a.Sum().ToString());")),
                    std::string("6"));
}

// ===========================================================================
// THE §26.5 RULE 1 CONTROL.
//
// `Array<T>` DECLARES `Contains` (stdlib §4.4), so §26.5 rule 1 commits
// to instance resolution for that name: "If `x`'s declared type (or any
// base type) declares any instance member named `Foo` ... instance
// resolution is committed and extension blocks are never consulted for that
// name on that receiver."  The instance member answers `True`; the user's
// extension is never the callee.
// ===========================================================================
BEZEL_TEST(user_extension_over_builtin_operator_7448,
           a_name_the_receiver_declares_is_answered_by_the_instance_member) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Contains(x: int): bool => false; end;",
        "  Console.WriteLine(a.Contains(2).ToString());")),
        std::string("True"));
}
