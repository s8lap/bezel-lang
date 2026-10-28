// campaign 2 (`builtin-call-admission`): the NARROWED RESIDUAL of issue
// #7285 — "`int.Parse` / `int.TryParse` silently accept a `uint`/`ulong` radix
// and SEGFAULT on a `bool`/`string` radix".
//
// THE FIX IS NOT THIS COMMIT'S. It is commit `d6722549` ("narrow #7285: an
// integer `Parse`/`TryParse` radix that binds to no `int` parameter is a
// located error, and no shape aborts the compiler"), which put the radix
// check on the declared `int` parameter of §9.1's radix forms
// (src/codegen/LlvmBackend_Calls2.cpp), and commit `2250daa5` ("fix #6825
// #7284 #7290 #7293 #7294 #7295 #7299 #7300 #7323 narrow #6812 #7372: anchor
// the built-in call admission on ONE declared-signature table"), which moved
// that refusal into the ONE declared-signature table and the resolver that
// consults it (`common::kBuiltinParams` +
// `ExprTC::checkBuiltinCallAdmission`). Both said `narrow`, so #7285 stayed
// OPEN with its content already fixed. THIS FILE IS THE PIN, AND THE
// `fix #7285` COMMIT IS THE CLOSURE.
//
// THE GOVERNING TEXT (checked in the CURRENT documents, v0.345).
//
// Standard library §9.1, docs/Bezel_Standard_Library.md:5161:
//
//   public static pure method Parse(s: string, radix: int): int;        // radix
//   in {2, 8, 10, 16}; any other
//
// and :5164:
//
//   public static method TryParse(s: string, radix: int, out result: int): bool;
//
// The `radix` parameter is an `int` in EVERY declaration.
//
// Language spec §5.1, docs/Bezel_Language_Spec.md:640 ("The following are
// **never implicit**"), and the never-implicit list's own bullet at
// docs/Bezel_Language_Spec.md:642 ("Between signed and unsigned types of the
// same or narrower width (e.g. `int → uint`, `uint → int`, `short → ushort`)"):
// a `uint` / `ulong` reaches an `int` parameter only as a COMPILE-TIME
// CONSTANT, by §5.1's constant narrowing (docs/Bezel_Language_Spec.md:647:
// "The rule is **value-based, not type-based**: the source may be a
// compile-time constant of **any** integer type — not only `int` — and the
// target any integer type whose range contains its value, **across
// signedness** ...; for non-constant expressions the conversion table and the
// never-implicit list above govern unchanged"). A `bool` or a `string` is
// never implicitly convertible to an `int` at all.
//
// Language spec §22.6, docs/Bezel_Language_Spec.md:4605:
//
//   If no candidate is applicable at all — no candidate is reachable: a
//   conversion §5.1 does not admit as implicit ... — the call is a compile
//   error with no candidates cited, because no overload was reachable.
//
// THE CLOSURE IS PARTLY A RULING, AND SAYS SO. A `uint`-TYPED **CONSTANT**
// radix IS admitted: §5.1's constant narrowing is value-based and crosses
// signedness, so `int.Parse("ff", 16u)` is well-typed and parses to 255. The
// row below pins that. Only a NON-CONSTANT `uint` / `ulong` radix is
// refused.
//
// THE ORACLE. A rejection is pinned by the diagnostic's TEXT — the declared
// stdlib form, the parameter NAME (`radix`), and the §5.1 rule the argument
// misses — plus a REAL LOCATION (`failedAtCompileTimeWithRealLocation`). A
// value is pinned by the program's STDOUT, never by the exit code.
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

// --- the radix argument: only a constant reaches the declared `int` ---

// A RUNTIME `uint` radix reaches no §9.1 parameter (§5.1's never-implicit
// list, at spec:642, governs non-constant expressions), so §22.6 makes the
// call a compile error. Before the fix this COMPILED and was silently
// accepted (the issue's silent-acceptance class).
BEZEL_TEST(narrowed_residual_7285,
           int_parse_with_a_runtime_uint_radix_is_refused) {
    auto r = runAot(prog("    var r: uint := 16;",
                         "    Console.WriteLine(int.Parse(\"ff\", r));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string, radix: int): int`, so the "
        "`radix` argument must be an `int` — a `uint` reaches an `int` "
        "parameter only as a compile-time constant (§5.1) — the call is a compile "
        "error with no candidates cited (§22.6)");
}

// The same, on the `TryParse` form and with a `ulong` radix.
BEZEL_TEST(narrowed_residual_7285,
           int_tryparse_with_a_runtime_ulong_radix_is_refused) {
    auto r = runAot(prog("    var r: ulong := 16;\n    var v: int := 0;",
                         "    _ := int.TryParse(\"ff\", r, out v);"));
    expectRefused(r,
        "stdlib §9.1 declares `TryParse(s: string, radix: int, out result: "
        "int): bool`, so the `radix` argument must be an `int` — a `ulong` "
        "reaches an `int` parameter only as a compile-time constant (§5.1) — the call "
        "is a compile error with no candidates cited (§22.6)");
}

// A `bool` radix reaches no `int` parameter under ANY reading of §5.1. Before
// the fix this COMPILED and the generated program SEGFAULTED (the issue's
// crash class).
BEZEL_TEST(narrowed_residual_7285, int_parse_with_a_bool_radix_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", true));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string, radix: int): int`, so the "
        "`radix` argument must be an `int` — a `bool` is never implicitly "
        "convertible to an `int` (§5.1) — the call is a compile error with no "
        "candidates cited (§22.6)");
}

// A `string` radix: likewise, and before the fix it SEGFAULTED too.
BEZEL_TEST(narrowed_residual_7285, int_parse_with_a_string_radix_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", \"16\"));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string, radix: int): int`, so the "
        "`radix` argument must be an `int` — a `string` is never implicitly "
        "convertible to an `int` (§5.1) — the call is a compile error with no "
        "candidates cited (§22.6)");
}

// --- the narrow's other two open items, in the same family ---

// THE FIRST-ARGUMENT RULE (named by `d6722549` as deliberately left open):
// stdlib §9.1 declares the first parameter as `s: string`, so an `int` first
// argument reaches no candidate. Before the anchor's admission gate this
// compiled and printed `0`.
BEZEL_TEST(narrowed_residual_7285,
           int_parse_with_an_int_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(16));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string): int` / "
        "`Parse(s: ReadOnlyStringSpan): int`, so the `s` argument must be a "
        "`string` — a `int` is never implicitly convertible to a `string` (§5.1) — "
        "the call is a compile error with no candidates cited (§22.6)");
}

// THE 1-ARGUMENT `Parse` ARM (named by `d6722549` as deliberately left open,
// and worse than "the same shape"): `int.Parse(true)` printed `1` before the
// gate — the wrong answer, from the `bool`'s own bytes.
BEZEL_TEST(narrowed_residual_7285,
           int_parse_with_a_bool_first_argument_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(true));"));
    expectRefused(r,
        "stdlib §9.1 declares `Parse(s: string): int` / "
        "`Parse(s: ReadOnlyStringSpan): int`, so the `s` argument must be a "
        "`string` — a `bool` is never implicitly convertible to a `string` (§5.1) — "
        "the call is a compile error with no candidates cited (§22.6)");
}

// --- the CONTROLS: the shapes the standard library ADMITS must not move ---

// §5.1's CONSTANT NARROWING: a `uint`-TYPED CONSTANT radix is admitted (the
// rule is value-based and crosses signedness), and the value parses.
BEZEL_TEST(narrowed_residual_7285,
           int_parse_with_a_uint_constant_radix_still_parses) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", 16u));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}

// The plain `int` radix, and the plain first argument.
BEZEL_TEST(narrowed_residual_7285, int_parse_with_an_int_radix_still_parses) {
    auto r = runAot(prog("", "    Console.WriteLine(int.Parse(\"ff\", 16));"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}
