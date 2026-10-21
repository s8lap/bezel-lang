// #6726 -- spec §35.5 *Case patterns* and §19.3, v0.340 (ruling #6578).
//
// §35.5 (v0.340): "A payload-free case is matched by its bare name … At a
// nested slot of a destructuring pattern the omission is read against the
// slot's declared type … A positional slot's naked identifier is a binding
// (§19.3), so a nested payload-free case or enum constant at a positional slot
// keeps its prefix, `Wrap(Outer.X)` and `Val(Color.Green)` -- the spelling the
// omission shortens." and: "A payload case may not be matched bare: `Fetched`
// without parentheses is a compile error naming the arity."
//
// The omission form was pinned (#4848, #2197); the PREFIXED spelling was not.
// MEASURED at base 97f4e965: every accepted prefixed shape below already ran
// correctly; the one gap was the bare PAYLOAD case in prefixed spelling
// (`Outcome.Fetched`, `Wrap(Outer.W)`, `Wrap(O = Outer.W)`), accepted and
// matched by tag instead of refused. The unprefixed `Fetched` was refused by
// checkTopLevelNamedPattern (1c) only. The reject rows fail without the fix.
//
// NOT pinned here (separate defect, #6600): a nested head that names nothing
// (`Wrap(Zzz(var c))`) or another choice (`Wrap(Outcome.Fetched(var c))` over
// an `Outer` slot) is accepted and runs the arm unconditionally.

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
    "choice GBox<T> GWrap(V: T), GNone; end GBox; "
    "choice Got Inner(Inner: Outcome), Nope; end Got; "
    "class Holder "
    "  public choice Inner A, B(N: int); end Inner; "
    "end Holder; "
    "choice HW HIn(I: Holder.Inner), HNone; end HW; ";

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

// Run `body`, which prints one line; the row asserts that exact stdout.
void expectOut(const std::string& body, const char* want) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

}  // namespace

// --- top level: choice-qualified payload-free case ---------------------------

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_case_statement_form) {
    expectOut("var o := Outcome.NotModified; "
              "case o of "
              "  Outcome.NotModified => Console.WriteLine(\"nm\"), "
              "  _ => Console.WriteLine(\"other\") "
              "end case; ",
              "nm\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_case_expression_form_mismatch) {
    expectOut("var o := Outcome.Fetched(200); "
              "var s := (case o of Outcome.NotModified => \"nm\", "
              "  Outcome.Fetched(var c) => $\"f{c}\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "f200\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_case_module_qualified) {
    expectOut("var o := Outcome.NotModified; "
              "var s := (case o of M.Outcome.NotModified => \"nm\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "nm\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_case_nested_in_enclosing_type) {
    expectOut("var i := Holder.Inner.A; "
              "var s := (case i of Holder.Inner.A => \"a\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "a\n");
}

// --- nested positional slot: the spelling the omission shortens -------------

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_case_selects_arm) {
    expectOut("var b := Box.Wrap(Outer.X); "
              "var s := (case b of Wrap(Outer.X) => \"x\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "x\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_case_mismatch_falls_through) {
    expectOut("var b := Box.Wrap(Outer.Y); "
              "var s := (case b of Wrap(Outer.X) => \"x\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "other\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_case_statement_form) {
    expectOut("var b := Box.Wrap(Outer.X); "
              "case b of "
              "  Wrap(Outer.X) => Console.WriteLine(\"x\"), "
              "  _ => Console.WriteLine(\"o\") "
              "end case; ",
              "x\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_payload_case_binds) {
    expectOut("var b := Box.Wrap(Outer.W(1, 2)); "
              "var s := (case b of Wrap(Outer.W(var a, var c)) => $\"{a}{c}\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "12\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_alternative_list) {
    expectOut("var b := Box.Wrap(Outer.Y); "
              "var s := (case b of Wrap(Outer.X | Outer.Y) => \"xy\", "
              "  _ => \"other\"); Console.WriteLine(s); ",
              "xy\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_prefixed_case_qualified_by_enclosing_type) {
    expectOut("var h := HW.HIn(Holder.Inner.B(5)); "
              "var s := (case h of HIn(Holder.Inner.A) => \"a\", "
              "  HIn(Holder.Inner.B(var n)) => $\"b{n}\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "b5\n");
}

// --- generic choice ---------------------------------------------------------

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_generic_scrutinee_nested_prefixed_case) {
    expectOut("var g := GBox<Outer>.GWrap(Outer.W(3, 4)); "
              "var s := (case g of GWrap(Outer.X) => \"x\", "
              "  GWrap(Outer.W(var a, var b)) => $\"{a}{b}\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "34\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_generic_scrutinee_nested_prefixed_case_mismatch) {
    expectOut("var g := GBox<Outer>.GWrap(Outer.Y); "
              "var s := (case g of GWrap(Outer.X) => \"x\", _ => \"other\"); "
              "Console.WriteLine(s); ",
              "other\n");
}

// --- named slot -------------------------------------------------------------

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_named_slot_prefixed_case) {
    expectOut("var g := Got.Inner(Outcome.NotModified); "
              "var s := (case g of Inner(Inner = Outcome.NotModified) => \"nm\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "nm\n");
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_named_slot_prefixed_payload_case) {
    expectOut("var g := Got.Inner(Outcome.Fetched(200)); "
              "var s := (case g of "
              "  Inner(Inner = Outcome.Fetched(Status = 200)) => \"ok\", "
              "  _ => \"o\"); Console.WriteLine(s); ",
              "ok\n");
}

// --- reject: a payload case matched bare in its prefixed spelling -----------
// §35.5: "A payload case may not be matched bare … a compile error naming the
// arity." Before the fix each of these compiled and matched by tag.

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_bare_payload_case_rejected) {
    auto r = runAot(prog(
        "var o := Outcome.Fetched(1); "
        "case o of "
        "  Outcome.Fetched => Console.WriteLine(\"x\"), "
        "  _ => Console.WriteLine(\"o\") "
        "end case; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `Fetched` requires a payload pattern"));
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_top_level_prefixed_bare_payload_case_expression_rejected) {
    auto r = runAot(prog(
        "var o := Outcome.NotModified; "
        "var s := (case o of Outcome.Fetched => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `Fetched` requires a payload pattern"));
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_positional_prefixed_bare_payload_case_rejected) {
    auto r = runAot(prog(
        "var b := Box.Wrap(Outer.W(1, 2)); "
        "var s := (case b of Wrap(Outer.W) => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `W` requires a payload pattern"));
}

BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_nested_named_prefixed_bare_payload_case_rejected_once) {
    auto r = runAot(prog(
        "var b := Box.Wrap(Outer.X); "
        "var s := (case b of Wrap(O = Outer.W) => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    std::size_t n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("Payload case `W` requires a payload pattern") !=
            std::string::npos)
            ++n;
    BEZEL_EXPECT_EQ(n, std::size_t(1));
}

// CONTROL: the unprefixed bare payload case was already refused.
BEZEL_TEST(sema_choice_prefixed_case_pattern_6726,
           spec_35_5_unprefixed_bare_payload_case_still_rejected) {
    auto r = runAot(prog(
        "var o := Outcome.NotModified; "
        "var s := (case o of Fetched => \"x\", _ => \"other\"); "
        "Console.WriteLine(s); "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "Payload case `Fetched` requires a payload pattern"));
}
