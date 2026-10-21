// #6788 -- spec §35.5 *Case patterns*, v0.340.
//
// §35.5 (v0.340): "At a nested slot of a destructuring pattern the omission is
// read against the slot's declared type ...: a nested head names a case of the
// choice that types the slot (`Wrap(W(var a, var b))` over a `Box` whose `Wrap`
// payload is an `Outer`, `W` being `Outer`'s case; ...)".
//
// MEASURED at base 37e4e291: `Wrap(Outcome.Fetched(var c))` over a `Box` whose
// `Wrap` payload is an `Outer` compiled, and the arm RAN for an `Outer.X`
// value (stdout `f`, exit 0). The reject rows fail without the fix; the accept
// rows (own-choice heads, prefixed, named slot, generic, nested type) pass on
// both sides.

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
    "choice Alt W(A: int, B: int), Z; end Alt; "
    "choice Box Wrap(O: Outer), Empty; end Box; "
    "choice GBox<T> GWrap(V: T), GNone; end GBox; "
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

void expectRejected(const std::string& body) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a case of `Outer`"));
}

}  // namespace

// --- reject -----------------------------------------------------------------

// The issue's repro: a case of ANOTHER choice, prefixed.
BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_case_of_other_choice_rejected) {
    expectRejected(
        "var b := Box.Wrap(Outer.X); "
        "var s := (case b of Wrap(Outcome.Fetched(var c)) => \"f\", "
        "  _ => \"o\"); Console.WriteLine(s); ");
}

// Same head with a sibling name that exists nowhere in `Outer`, unprefixed.
BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_unprefixed_non_case_rejected) {
    expectRejected(
        "var b := Box.Wrap(Outer.X); "
        "var s := (case b of Wrap(Fetched(var c)) => \"f\", _ => \"o\"); "
        "Console.WriteLine(s); ");
}

// The last segment IS a case of the slot's choice, the prefix is another
// choice: `Alt.W` is not `Outer.W`.
BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_wrong_prefix_same_case_name_rejected) {
    expectRejected(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "var s := (case b of Wrap(Alt.W(var a, var c)) => \"w\", _ => \"o\"); "
        "Console.WriteLine(s); ");
}

BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_case_of_other_choice_statement_form_rejected) {
    expectRejected(
        "var b := Box.Wrap(Outer.X); "
        "case b of "
        "  Wrap(Outcome.Fetched(var c)) => Console.WriteLine(\"f\"), "
        "  _ => Console.WriteLine(\"o\") "
        "end case; ");
}

// Named slot: `O = Outcome.Fetched(...)` over an `Outer` slot.
BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_case_of_other_choice_named_slot_rejected) {
    expectRejected(
        "var b := Box.Wrap(Outer.X); "
        "var s := (case b of Wrap(O = Outcome.Fetched(Status = var c)) => "
        "  \"f\", _ => \"o\"); Console.WriteLine(s); ");
}

// --- accept controls --------------------------------------------------------

BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_own_choice_unprefixed_accepted) {
    expectOut("var b := Box.Wrap(Outer.W(1, 2)); "
              "var s := (case b of Wrap(W(var a, var c)) => $\"{a}{c}\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "12\n");
}

BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_own_choice_prefixed_accepted) {
    expectOut("var b := Box.Wrap(Outer.W(1, 2)); "
              "var s := (case b of Wrap(Outer.W(var a, var c)) => "
              "  $\"{a}{c}\", _ => \"o\"); Console.WriteLine(s); ",
              "12\n");
}

BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_own_choice_named_slot_accepted) {
    expectOut("var g := Got.Inner(Outcome.Fetched(200)); "
              "var s := (case g of "
              "  Inner(Inner = Outcome.Fetched(Status = 200)) => \"ok\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "ok\n");
}

BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_generic_slot_own_choice_accepted) {
    expectOut("var g := GBox<Outer>.GWrap(Outer.W(3, 4)); "
              "var s := (case g of GWrap(Outer.W(var a, var b)) => "
              "  $\"{a}{b}\", _ => \"other\"); Console.WriteLine(s); ",
              "34\n");
}

// A mismatching VALUE with the right head still falls through (runtime, not
// a compile error).
BEZEL_TEST(sema_choice_nested_head_other_choice_6788,
           spec_35_5_nested_head_own_choice_other_case_falls_through) {
    expectOut("var b := Box.Wrap(Outer.X); "
              "var s := (case b of Wrap(Outer.W(var a, var c)) => \"w\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "o\n");
}
