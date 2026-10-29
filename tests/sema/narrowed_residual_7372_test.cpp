// campaign 2 (`builtin-call-admission`): the NARROWED RESIDUAL of issue
// #7372 — "the `Parse`/`TryParse` family ADMITS a non-string first argument
// and silently answers false".
//
// THE FIX IS NOT THIS COMMIT'S. It is commit `2250daa5` ("fix #6825 #7284
// #7290 #7293 #7294 #7295 #7299 #7300 #7323 narrow #6812 #7372: anchor the
// built-in call admission on ONE declared-signature table"), which put the
// built-in call admission on ONE declared-signature table
// (`common::kBuiltinParams`, src/common/BuiltinRegistry.cpp) and resolved
// every built-in call against the standard library's DECLARED signature
// (`ExprTC::checkBuiltinCallAdmission`, src/sema/ExprTypecheck_Calls.cpp).
// That commit said `narrow`, so #7372 stayed OPEN with its content already
// fixed. THIS FILE IS THE PIN, AND THE `fix #7372` COMMIT IS THE CLOSURE.
//
// THE GOVERNING TEXT (checked in the CURRENT documents, v0.345).
//
// Language spec §22.6, docs/Bezel_Language_Spec.md:4605:
//
//   If no candidate is applicable at all — no candidate is reachable: a
//   conversion §5.1 does not admit as implicit, a parameter-count difference
//   at a function-typed slot, a method-reference formation that fails, or an
//   inference that finds no binding — the call is a compile error with no
//   candidates cited, because no overload was reachable.
//
// Language spec §5.1, docs/Bezel_Language_Spec.md:640 ("The following are
// **never implicit**"), and the never-implicit list's own bullet at
// docs/Bezel_Language_Spec.md:642 ("Between signed and unsigned types of the
// same or narrower width (e.g. `int → uint`, `uint → int`, `short → ushort`)"):
// NO conversion to `string` is admitted from an `int`, a `double`, a `bool`,
// a `char`, an `Object` or an `Object?`.
//
// The standard library declares the first parameter of every `Parse` /
// `TryParse` form as a `string`:
//   - stdlib §9.1 (`int`), docs/Bezel_Standard_Library.md
//     `public static pure method Parse(s: string): int;`
//     and :5153 `public static method TryParse(s: string, out result: int): bool;`
//     (the `ReadOnlyStringSpan` forms at :5170-5171);
//   - stdlib §9.2 (`double`), docs/Bezel_Standard_Library.md;
//   - stdlib §9.3 (`decimal`), docs/Bezel_Standard_Library.md;
//   - stdlib §9.4 (`bool`), docs/Bezel_Standard_Library.md;
//   - stdlib §9.5 (`char`), docs/Bezel_Standard_Library.md;
//   - stdlib §2.6 (`Guid`), docs/Bezel_Standard_Library.md.
//
// So a call that reaches one of those forms with a non-string first argument
// has NO reachable candidate, and §22.6 makes it a compile error.
//
// THE ORACLE. A rejection is pinned by the diagnostic's TEXT — the declared
// stdlib form, the parameter NAME, and the position — plus a REAL LOCATION
// (`failedAtCompileTimeWithRealLocation`). Asserting "§22.6" alone would be
// vacuous: every one of these rejections says §22.6. A value is pinned by the
// program's STDOUT.
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

// --- the Parse family: a non-string first argument has no reachable candidate ---

// stdlib §9.1 declares `Parse(s: string): int` and the span form. An `int`
// reaches neither §9.1 parameter (§5.1 admits no conversion to `string`), so
// §22.6 makes the call a compile error. Before the anchor it COMPILED and
// printed `0` with an `internal:` degrade — the silent-acceptance inversion
// #7372 exists for.
BEZEL_TEST(narrowed_residual_7372, int_parse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(16));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string): int` / "
        "`Parse(s: ReadOnlyStringSpan): int`, so the `s` argument must be a "
        "`string` — a `int` is never implicitly convertible to a `string` (§5.1) — "
        "the call is a compile error with no candidates cited (§22.6)");
}

// The `TryParse` form of §9.1: `TryParse(s: string, out result: int): bool`
// — the first parameter is still a `string`.
BEZEL_TEST(narrowed_residual_7372,
           int_tryparse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("    var v: int := 0;",
                         "    _ := int.TryParse(16, out v);"));
    expectRefused(r,
        "stdlib §9.1 declares `TryParse(s: string, out result: int): bool` / "
        "`TryParse(s: ReadOnlyStringSpan, out result: int): bool`, so the `s` "
        "argument must be a `string` — a `int` is never implicitly convertible "
        "to a `string` (§5.1) — the call is a compile error with no candidates cited "
        "(§22.6)");
}

// stdlib §9.4 (`bool`): `Parse(s: string): bool`.
BEZEL_TEST(narrowed_residual_7372, bool_parse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(bool.Parse(1));"));
    expectRefused(r,
        "stdlib §9.4 declares `Parse(s: string): bool`, so the `s` argument "
        "must be a `string` — a `int` is never implicitly convertible to a "
        "`string` (§5.1) — the call is a compile error with no candidates cited "
        "(§22.6)");
}

// stdlib §9.5 (`char`): `TryParse(s: string, out result: char): bool`.
BEZEL_TEST(narrowed_residual_7372,
           char_tryparse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("    var v: char := 'a';",
                         "    _ := char.TryParse(1, out v);"));
    expectRefused(r,
        "stdlib §9.5 declares `TryParse(s: string, out result: char): bool`, "
        "so the `s` argument must be a `string` — a `int` is never implicitly "
        "convertible to a `string` (§5.1) — the call is a compile error with no "
        "candidates cited (§22.6)");
}

// stdlib §9.2 (`double`): `Parse(s: string): double`.
BEZEL_TEST(narrowed_residual_7372,
           double_parse_with_a_double_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(double.Parse(1.5));"));
    expectRefused(r,
        "stdlib §9.2 declares `Parse(s: string): double` / "
        "`Parse(s: ReadOnlyStringSpan): double`, so the `s` argument must be a "
        "`string` — a `double` is never implicitly convertible to a `string` (§5.1) — "
        "the call is a compile error with no candidates cited (§22.6)");
}

// stdlib §9.3 (`decimal`): `TryParse(s: string, out result: decimal): bool`.
BEZEL_TEST(narrowed_residual_7372,
           decimal_tryparse_with_a_double_first_argument_is_refused) {
    auto r = runAot(prog("    var m: decimal := 0m;",
                         "    _ := decimal.TryParse(1.5, out m);"));
    expectRefused(r,
        "stdlib §9.3 declares `TryParse(s: string, out result: decimal): bool` "
        "/ `TryParse(s: ReadOnlyStringSpan, out result: decimal): bool`, so "
        "the `s` argument must be a `string` — a `double` is never implicitly "
        "convertible to a `string` (§5.1) — the call is a compile error with no "
        "candidates cited (§22.6)");
}

// stdlib §2.6 (`Guid`): `TryParse(s: string, out result: Guid): bool`.
BEZEL_TEST(narrowed_residual_7372,
           guid_tryparse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("    var g: Guid := Guid.Empty;",
                         "    _ := Guid.TryParse(1, out g);"));
    expectRefused(r,
        "stdlib §2.6 declares `TryParse(s: string, out result: Guid): bool`, "
        "so the `s` argument must be a `string` — a `int` is never implicitly "
        "convertible to a `string` (§5.1) — the call is a compile error with no "
        "candidates cited (§22.6)");
}

// The `null` LITERAL has type `Object?`, and §5.1's nullable lifting
// converts a `T` to a `T?`, never the other way — so it reaches no `string`
// parameter either. (Before the anchor this compiled and printed `0`.)
BEZEL_TEST(narrowed_residual_7372, long_parse_with_a_null_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(long.Parse(null));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string): long` / "
        "`Parse(s: ReadOnlyStringSpan): long`, so the `s` argument must be a "
        "`string` — a `Object?` is never implicitly convertible to a `string` "
        "(§5.1's nullable lifting converts a `T` to a `T?`, never the other "
        "way) (§5.1) — the call is a compile error with no candidates cited (§22.6)");
}

// A CLASS INSTANCE reaches no §9.1 parameter either — and before the anchor
// the compiler lowered the object's POINTER as a string and read FOREIGN
// MEMORY as text.
BEZEL_TEST(narrowed_residual_7372,
           int_parse_with_an_object_first_argument_is_refused) {
    auto r = runAot(prog("    var o: Object := 1;",
                         "    Console.WriteLine(int.Parse(o));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string): int` / "
        "`Parse(s: ReadOnlyStringSpan): int`, so the `s` argument must be a "
        "`string` — a `Object` is never implicitly convertible to a `string` (§5.1) — "
        "the call is a compile error with no candidates cited (§22.6)");
}

// --- the CONTROLS: the shapes the standard library ADMITS must not move ---

// stdlib §9.1's `Parse(s: string): int` — the value, not the exit code.
BEZEL_TEST(narrowed_residual_7372, int_parse_with_a_string_first_argument_still_parses) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"16\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "16\n");
}

// stdlib §9.2 / §9.3 / §9.4 / §9.5: the other receivers' string forms.
BEZEL_TEST(narrowed_residual_7372, the_other_parse_families_still_parse) {
    auto r = runAot(prog("", "    Console.WriteLine(double.Parse(\"1.5\"));\n"
                            "    Console.WriteLine(decimal.Parse(\"1.5\"));\n"
                            "    Console.WriteLine(bool.Parse(\"true\"));\n"
                            "    Console.WriteLine(char.Parse(\"a\"));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.5\n1.5\nTrue\na\n");
}

// stdlib §9.1's `TryParse(s: string, out result: int): bool` — the out slot
// carries the parsed value.
BEZEL_TEST(narrowed_residual_7372, int_tryparse_with_a_string_first_argument_still_parses) {
    auto r = runAot(prog("    var v: int := 0;",
                         "    Console.WriteLine(int.TryParse(\"16\", out v));\n"
                         "    Console.WriteLine(v);"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n16\n");
}
