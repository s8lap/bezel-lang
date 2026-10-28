// campaign 2 (`builtin-call-admission`): the NARROWED RESIDUAL of issue
// #7333 — "`bool.ToString("F4")` and `char.ToString("F4")` COMPILE — stdlib
// §2.18 says a format spec on `bool`/`char` IS A COMPILE ERROR".
//
// THE FIX IS NOT THIS COMMIT'S. It is commit `cc7d9855` ("narrow #7333: a
// format spec on a bool or char receiver is a compile error"), which added
// the shared type-formattability predicate (`src/sema/Formattability.hpp`)
// and consulted it from BOTH spellings — the member call and the
// interpolation hole — so the two spellings of ONE rule agree by
// construction. That commit said `narrow`, so #7333 stayed OPEN with its
// content already fixed. THIS FILE IS THE PIN, AND THE `fix #7333` COMMIT IS
// THE CLOSURE.
//
// THE GOVERNING TEXT (checked in the CURRENT documents, v0.345).
//
// Standard library §2.18, docs/Bezel_Standard_Library.md:1144:
//
//   The primitive and temporal types are **intrinsically formattable** (they
//   behave as conforming implementations without declaring the interface); a
//   format spec on any other non-conforming operand is a compile error.
//
// and the specifier vocabulary's last row,
// docs/Bezel_Standard_Library.md:1156:
//
//   | `bool`, `char`, `string` | *(none)* | a format spec on these is a
//   compile error |
//
// Language spec §18.5, docs/Bezel_Language_Spec.md:3799:
//
//   A format spec on an operand whose type is neither intrinsically
//   formattable nor `IFormattable` is a **compile error** naming this rule.
//
// and §18.5's nullable clause, docs/Bezel_Language_Spec.md:3801:
//
//   A format spec on a nullable-typed hole is checked at compile time against
//   the **non-nullable form** of the operand's type under the dispatch rule
//   above (the type-formattability check ...)
//
// The `string` receiver is the third member of that vocabulary row, and it is
// refused by the rule that owns it: stdlib §9.6 declares
// `public pure override method ToString(): string;` (no format parameter),
// docs/Bezel_Standard_Library.md:5454, so the one-argument form is an arity
// error under spec §22.5.
//
// THE ORACLE. A rejection is pinned by the diagnostic's TEXT — the whole
// governing sentence, with the receiver's type named — plus a REAL LOCATION
// (`failedAtCompileTimeWithRealLocation`). A value is pinned by the program's
// STDOUT, never by the exit code.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// The rejection oracle: the named diagnostic, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const std::string& needle) {
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program the row runs. `pre` is the locals; `body` the statements.
std::string prog(const std::string& pre, const std::string& body) {
    return R"(
module M;
class C
  public static method Main(): int
)" + pre + R"(
)" + body + R"(
    return 0;
  end Main;
end C;
)";
}

}  // namespace

// --- the reject rows: a format spec on a receiver with no vocabulary ---

// stdlib §2.18's `bool` row: the vocabulary is empty, so a format spec on a
// `bool` is a compile error. This compiled and printed "True" before the fix.
BEZEL_TEST(narrowed_residual_7333, bool_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("    var b := true;",
                         "    Console.WriteLine(b.ToString(\"F4\"));"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable receiver "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`bool` is neither (§2.18; §18.5)");
}

// stdlib §2.18's `char` row: the same empty vocabulary. This compiled and
// printed "a" before the fix.
BEZEL_TEST(narrowed_residual_7333, char_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("    var c := 'a';",
                         "    Console.WriteLine(c.ToString(\"F4\"));"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable receiver "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`char` is neither (§2.18; §18.5)");
}

// spec §18.5's NULLABLE clause, read at a receiver: §7.4 narrows the local
// to non-null, and the narrowed read is checked as the NON-NULLABLE form of
// the operand's type — `bool`.
BEZEL_TEST(narrowed_residual_7333,
           narrowed_bool_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("    var b: bool? := true;",
                         "    if b <> null then Console.WriteLine(b.ToString(\"F4\")); end;"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable receiver "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`bool` is neither (§2.18; §18.5)");
}

// The same, for `char`.
BEZEL_TEST(narrowed_residual_7333,
           narrowed_char_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("    var c: char? := 'a';",
                         "    if c <> null then Console.WriteLine(c.ToString(\"F4\")); end;"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable receiver "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`char` is neither (§2.18; §18.5)");
}

// stdlib §2.18:1144's own sentence — "a format spec on any other
// non-conforming operand is a compile error". An `Object` is in no
// vocabulary row and implements no `IFormattable`.
BEZEL_TEST(narrowed_residual_7333, object_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("    var o: Object := true;",
                         "    Console.WriteLine(o.ToString(\"F4\"));"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable receiver "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`Object` is neither (§2.18; §18.5)");
}

// The HOLE spelling of the same rule: spec §18.5's dispatch paragraph, whose
// diagnostic names the OPERAND rather than the receiver.
BEZEL_TEST(narrowed_residual_7333, hole_with_a_format_spec_on_a_bool_is_refused) {
    auto r = runAot(prog("    var b := true;",
                         "    Console.WriteLine($\"{b:F4}\");"));
    expectRefused(r,
        "a format spec requires an intrinsically-formattable operand "
        "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
        "`bool` is neither (§18.5)");
}

// The `string` receiver: stdlib §9.6 declares `ToString(): string` — no
// format parameter (docs/Bezel_Standard_Library.md:5454) — so the
// one-argument form is an arity error, under spec §22.5's rule that owns it.
BEZEL_TEST(narrowed_residual_7333, string_receiver_with_a_format_spec_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(\"abc\".ToString(\"F4\"));"));
    expectRefused(r,
        "the call to `ToString` on `string` supplies 1 argument(s) but stdlib "
        "§9.6 declares `ToString(): string` — exactly 0 argument(s) "
        "(§22.5; §9.6)");
}

// --- the CONTROLS: the shapes the standard library ADMITS must not move ---

// stdlib §2.18's integer row: `D`/`Dn` is a decimal specifier, so an `int`
// receiver admits it and renders the zero-padded digits.
BEZEL_TEST(narrowed_residual_7333, int_receiver_with_a_format_spec_renders) {
    auto r = runAot(prog("", "    Console.WriteLine((255).ToString(\"D4\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0255\n");
}

// stdlib §2.18's `bool` / `char` rows govern the SPEC, not the no-format
// `ToString()`: the empty vocabulary rejects a spec, not the member.
BEZEL_TEST(narrowed_residual_7333,
           bool_and_char_receivers_without_a_format_spec_render) {
    auto r = runAot(prog("", "    Console.WriteLine((true).ToString());\n"
                            "    Console.WriteLine(('a').ToString());"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\na\n");
}
