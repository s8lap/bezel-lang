// #6787 -- spec §35.5 *Case patterns*, v0.340, and Appendix C.7.
//
// §35.5 (v0.340): "A payload case is matched by a positional destructuring
// pattern `CaseName(slot, ...)` with the case's exact payload arity"; "`_`
// discards per slot". C.7: "the same arity rule applies to choice-case
// destructuring".
//
// MEASURED at base 83afc22b~1: with `choice Outer X, Y, W(A: int, B: int)`,
// the NESTED patterns `Wrap(W(_))` and `Wrap(Outer.W(_))` compiled and matched
// (stdout `x`, exit 0): the top-level callers check the slot count, the nested
// walk did not. The bare-case refusal's hint also offered `W(_)` for the
// two-slot `W`, the same wrong-arity pattern. The reject rows and the hint row
// fail without the fix; the accept rows pass on both sides.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char kDecls[] =
    "module M; "
    "choice Outcome NotModified, Fetched(Status: int), Failed(Msg: string); "
    "end Outcome; "
    "choice Outer X, Y, W(A: int, B: int); end Outer; "
    "choice Box Wrap(O: Outer), Empty; end Box; "
    "choice Got Inner(Inner: Outcome), Nope; end Got; ";

std::string prog(const std::string& body) {
    return std::string(kDecls) +
           "public class Program "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectOut(const std::string& body, const char* want) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

void expectArityRejected(const std::string& body, const char* binds) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, binds));
    BEZEL_EXPECT_TRUE(diagContains(r, "the slot count must match"));
}

}  // namespace

// --- reject: nested slot count differs from the payload arity ----------------

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_one_slot_over_two_slot_case_unprefixed_rejected) {
    expectArityRejected(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "var s := (case b of Wrap(W(_)) => \"x\", _ => \"o\"); "
        "Console.WriteLine(s); ",
        "binds 1 slot(s) but case `W` has 2 payload field(s)");
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_one_slot_over_two_slot_case_prefixed_rejected) {
    expectArityRejected(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "var s := (case b of Wrap(Outer.W(_)) => \"x\", _ => \"o\"); "
        "Console.WriteLine(s); ",
        "binds 1 slot(s) but case `W` has 2 payload field(s)");
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_too_many_slots_rejected) {
    expectArityRejected(
        "var g := Got.Inner(Outcome.Fetched(1)); "
        "var s := (case g of Inner(Fetched(_, _)) => \"x\", _ => \"o\"); "
        "Console.WriteLine(s); ",
        "binds 2 slot(s) but case `Fetched` has 1 payload field(s)");
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_one_slot_over_two_slot_case_statement_rejected) {
    expectArityRejected(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "case b of "
        "  Wrap(W(_)) => Console.WriteLine(\"x\"), "
        "  _ => Console.WriteLine(\"o\") "
        "end case; ",
        "binds 1 slot(s) but case `W` has 2 payload field(s)");
}

// --- the bare-case hint must not offer the wrong-arity pattern ---------------

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_bare_two_slot_case_hint_discards_every_slot) {
    auto r = runAot(prog(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "var s := (case b of Wrap(Outer.W) => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "or `W(_, _)` to discard"));
    BEZEL_EXPECT_TRUE(!diagContains(r, "`W(_)`"));
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_bare_one_slot_case_hint_keeps_single_discard) {
    auto r = runAot(prog(
        "var o := Outcome.NotModified; "
        "var s := (case o of Fetched => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "or `Fetched(_)` to discard"));
}

// --- accept controls ---------------------------------------------------------

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_all_discard_per_slot_accepted) {
    expectOut("var b := Box.Wrap(Outer.W(1, 2)); "
              "var s := (case b of Wrap(W(_, _)) => \"x\", _ => \"o\"); "
              "Console.WriteLine(s); ",
              "x\n");
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_exact_arity_binds_accepted) {
    expectOut("var b := Box.Wrap(Outer.W(1, 2)); "
              "var s := (case b of Wrap(Outer.W(var a, var c)) => "
              "  $\"{a}{c}\", _ => \"o\"); Console.WriteLine(s); ",
              "12\n");
}

BEZEL_TEST(sema_choice_nested_case_arity_6787,
           spec_35_5_nested_one_slot_case_single_discard_accepted) {
    expectOut("var g := Got.Inner(Outcome.Fetched(7)); "
              "var s := (case g of Inner(Fetched(_)) => \"x\", _ => \"o\"); "
              "Console.WriteLine(s); ",
              "x\n");
}
