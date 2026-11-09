// Spec-derived regression test for issues #8294 and #8295.
//
// SPEC §27.6 (v0.355), *Method references to overloaded methods*, verbatim:
//
//   "When the named method (or property) is overloaded, the reference is
//    resolved using the **target function type** at the use site."
//
// SPEC §27.6 (v0.355), *References to generic methods*, verbatim:
//
//   "Overload selection follows §14.5's list rule: with a list, only
//    candidates declaring that many type parameters of their own take part;
//    without one, only candidates declaring none take part, and the error
//    above is reported where every candidate is generic."
//
// SPEC §26.5 rule 4 / §26.5.1 (v0.355): two or more equally specific
// applicable extension blocks are a compile error; §26.5.1 ranks "the
// APPLICABLE candidates", and an extension member "matches the call's
// arity" — so the rule-4 tie is a PER-ARITY fact.
//
// THE DEFECTS (measured on this fix's base):
//   A. (#8295) §14.5's list rule reached the CLASS-member path only. Over an
//      EXTENSION member the rule was never applied: a reference with no list
//      denoted the GENERIC member, the generic member's own signature became
//      the reference's type, and the call through it was refused ("call
//      supplies 0 argument(s) but the function value expects 1", §5.1).
//   B. (#8294) the rule-4 tie was read off the member NAME — the bare-
//      reference lookup asked for "any member named `Len`" and reported the
//      tie between the blocks before the target function type had selected
//      an arity. A tie at an arity the target does NOT select was reported
//      as a tie, and the reference was refused.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

std::size_t errorCount(const JitResult& r, const std::string& needle) {
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        for (std::size_t at = d.message.find(needle); at != std::string::npos;
             at = d.message.find(needle, at + needle.size()))
            ++n;
    }
    return n;
}

const char kTieAmbiguity[] =
    "ambiguous extension member `Len`: 2 extensions are equally specific";

// A. (#8295) the §14.5 list rule over EXTENSION members.
//
// The generic member sits in the MORE SPECIFIC block (`extension Array<T>`)
// and the non-generic one in the less specific block (`extension
// IEnumerable<T>`), so the §26.5.1 lookup picks the generic member — and
// without the list rule the reference denotes it.
const char kGenericOverNonGeneric[] =
    "extension IEnumerable<T>\n"
    "  public method Len(): int => 1;\n"
    "end;\n"
    "extension Array<T>\n"
    "  public method Len<U>(x: U): int => 200;\n"
    "end;\n";

// The mirror: the generic member in the LESS specific block and the
// non-generic one in the more specific block. The list rule decides the
// same way whichever block declares what (§27.6: "without one, only
// candidates declaring none take part").
const char kNonGenericOverGeneric[] =
    "extension IEnumerable<T>\n"
    "  public method Len<U>(x: U): int => 200;\n"
    "end;\n"
    "extension Array<T>\n"
    "  public method Len(): int => 1;\n"
    "end;\n";

std::string extensionMain(const std::string& blocks, const std::string& body) {
    return std::string("module M;\n") + blocks +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectCheckedStdout(const std::string& src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectRejected(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// ── A (#8295): §14.5's list rule reaches the extension path ─────────────────
//
// The reference has NO type-argument list, so §27.6's list rule keeps only
// the candidates declaring NO type parameters of their own — the non-generic
// `Len(): int`. The reference denotes it, its type is `() -> int`, and the
// call through it supplies 0 argument(s).

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           generic_extension_member_does_not_take_part_without_a_list) {
    // per §27.6 / §14.5 (v0.355): the issue's literal repro
    expectCheckedStdout(
        extensionMain(kGenericOverNonGeneric,
                      "var h := a.Len;\n"
                      "    Console.WriteLine(h());"),
        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           generic_extension_member_does_not_take_part_with_a_target) {
    // per §27.6 / §14.5 (v0.355): the same rule with a target function type
    expectCheckedStdout(
        extensionMain(kGenericOverNonGeneric,
                      "var h: () -> int := a.Len;\n"
                      "    Console.WriteLine(h());"),
        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_list_rule_is_independent_of_which_block_declares_the_generic) {
    // per §27.6 / §14.5 (v0.355): "without one, only candidates declaring
    // none take part" — whichever block declares what
    expectCheckedStdout(
        extensionMain(kNonGenericOverGeneric,
                      "var h := a.Len;\n"
                      "    Console.WriteLine(h());"),
        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_generic_member_is_not_selected_where_every_candidate_is_generic) {
    // control: the list rule leaves NO candidate, so the reference is still
    // the generic member's — and the generic-thunk refusal stands
    expectRejected(
        extensionMain("extension Array<T>\n"
                      "  public method Len<U>(x: U): int => 200;\n"
                      "end;\n",
                      "var h := a.Len;\n"
                      "    Console.WriteLine(h);"),
        "the reference to the generic method `Len` cannot be formed");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_generic_extension_member_is_not_the_references_type_at_a_print) {
    // per §27.6 / §14.5 (v0.355): a `Console.WriteLine` position supplies no
    // function type, so the reference is the non-generic member's — the
    // printed type is the reference's own, `() -> int`, not the generic
    // member's `(U) -> int` (which the base refused outright).
    expectCheckedStdout(
        extensionMain(kGenericOverNonGeneric, "Console.WriteLine(a.Len);"),
        "() -> int\n");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_class_member_list_rule_is_unchanged) {
    // control: the §14.5 list rule over a CLASS member's overload set — the
    // path that already applied it (#8205)
    expectCheckedStdout(
        "module M;\n"
        "class V\n"
        "  public constructor() end constructor;\n"
        "  public method Len<T>(x: T): int => 7;\n"
        "  public method Len(): int => 42;\n"
        "end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var v := new V();\n"
        "    var f := v.Len;\n"
        "    Console.WriteLine(f());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "42\n");
}

// ── B (#8294): the rule-4 tie is read at the arity the reference selects ─────
//
// `extension IEnumerable<int>` declares `Len(x: int)` and `Len()`, and
// `extension IEnumerable<T>` declares `Len(x: int)`. The two BLOCKS are
// equally specific (§26.5.1), so the lookup's per-block scan — which picks
// each block's FIRST same-name member regardless of arity — sees a tie. But
// the tie is a PER-ARITY fact: at arity 0 only `IEnumerable<int>`'s `Len()`
// applies, at arity 1 the two `Len(x: int)` members tie.

namespace {

const char kTieBlocks[] =
    "extension IEnumerable<int>\n"
    "  public method Len(x: int): int => x + 200;\n"
    "  public method Len(): int => 1;\n"
    "end;\n"
    "extension IEnumerable<T>\n"
    "  public method Len(x: int): int => x + 300;\n"
    "end;\n";

}  // namespace

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           tie_at_another_arity_does_not_reject_the_selected_one) {
    // per §26.5 rule 4 / §26.5.1 / §27.6 (v0.355): the issue's literal repro
    expectCheckedStdout(
        extensionMain(kTieBlocks,
                      "var h: () -> int := a.Len;\n"
                      "    Console.WriteLine(h());"),
        "1\n");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           tie_at_the_selected_arity_is_still_rejected) {
    // control: the same blocks, and the target selects the arity at which
    // they DO tie (§26.5.1's ranking over the applicable candidates)
    expectRejected(extensionMain(kTieBlocks,
                                 "var g: (int) -> int := a.Len;\n"
                                 "    Console.WriteLine(g(5));"),
                   kTieAmbiguity);
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_tie_at_the_selected_arity_is_reported_once) {
    // control (fix #8205's one-report-per-reference discipline): the tie is
    // ONE diagnostic for the reference
    auto r = runAot(extensionMain(kTieBlocks,
                                  "var g: (int) -> int := a.Len;\n"
                                  "    Console.WriteLine(g(5));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_EQ(errorCount(r, kTieAmbiguity), static_cast<std::size_t>(1));
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           the_call_of_the_tied_arity_is_the_same_ambiguity) {
    // control: the call the reference resolves as — §26.5 rule 4's error at
    // the arity, in the lookup's own wording
    expectRejected(extensionMain(kTieBlocks, "Console.WriteLine(a.Len(5));"),
                   kTieAmbiguity);
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           a_reference_with_no_target_at_a_tied_arity_is_still_rejected) {
    // control: with no target the reference selects no arity, so a tie at
    // any arity it could select is a tie that matters (§26.5 rule 4)
    expectRejected(extensionMain("extension IEnumerable<int>\n"
                                 "  public method Len(x: int): int => x + 200;\n"
                                 "end;\n"
                                 "extension IEnumerable<T>\n"
                                 "  public method Len(x: int): int => x + 300;\n"
                                 "end;\n",
                                 "var f := a.Len;\n"
                                 "    Console.WriteLine(f);"),
                   kTieAmbiguity);
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           a_reference_with_no_target_over_an_overloaded_set_is_still_ambiguous) {
    // control: §27.6's no-target rule — the candidate set is overloaded, so
    // the reference is ambiguous whatever the tie says
    expectRejected(extensionMain(kTieBlocks,
                                 "var f := a.Len;\n"
                                 "    Console.WriteLine(f);"),
                   "the method reference `Len` is overloaded and has no target "
                   "type to select by");
}

BEZEL_TEST(sema_method_ref_overload_target_selection_r41,
           a_more_specific_block_still_wins_at_the_selected_arity) {
    // control: §26.5 rule 3 — the more specific block's member is selected
    expectCheckedStdout(
        extensionMain("extension Array<int>\n"
                      "  public method Len(): int => 1;\n"
                      "end;\n"
                      "extension IEnumerable<int>\n"
                      "  public method Len(x: int): int => x + 200;\n"
                      "end;\n",
                      "var g: (int) -> int := a.Len;\n"
                      "    Console.WriteLine(g(5));"),
        "205\n");
}
