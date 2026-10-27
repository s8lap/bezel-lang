// fix #7191 — the `string.Join` SEPARATOR gate must read the argument that
// binds to the `separator` PARAMETER, not the argument at position 0.
//
// §22.5 (v0.343) says: "named arguments may appear in any order after
// positional arguments. Each supplied argument binds exactly one parameter".
// stdlib §9.6 (v0.343) declares
//
//     public static method Join(separator: string, items: IEnumerable<string>): string;
//
// — `separator` is parameter 0, `items` is parameter 1.
//
// THE DEFECT. The #7150 separator gate (src/sema/ExprTypecheck_Calls.cpp)
// read `ce.args[0]` POSITIONALLY, so it checked the FIRST WRITTEN argument as
// if it were the separator:
//
//     string.Join(separator := ",", items := parts)   → ACCEPTED, prints a,b,c
//     string.Join(items := parts, separator := ",")   → REJECTED
//
// The two spellings name the same parameters and supply the same arguments —
// they are the same call — yet they got OPPOSITE verdicts, and the diagnostic
// MISLED: the reader named `items` first and the compiler complained about
// the separator ("the separator argument to `string.Join` has a type that is
// not implicitly convertible to the target type (§5.1): the built-in type
// `Array` is a heap reference ..."), naming a parameter the reader never
// supplied a value for.
//
// MEASURED pre-fix on this tree (the direct parent of this fix), on a fresh
// `bezelc`:
//
//   reordered   → "reordered.bzl:5:44: the separator argument to `string.Join`
//                 has a type that is not implicitly convertible to the target
//                 type (§5.1): the built-in type `Array` is a heap reference
//                 and the target is a value type ..."  (REJECTED)
//   in-order   → prints "a,b,c"
//   positional → prints "a,b,c"
//
// ⇒ the fix is the SEMANTIC half of #7165: the CODEGEN half (the static
// `string.Join` arm reads the published §22.5 binding — #7165's pick,
// e19de750) is already on this tree, so the reordered call now computes the
// right value; what was left was the front end rejecting it.
//
// THE VALUE, NOT THE EXIT CODE. Every accept row asserts the program's STDOUT
// (the VALUE channel): a process exit status is masked to 0..255 and cannot
// carry a string value, so an exit-code assertion could not tell "a,b,c" from
// "b,c,a".
//
// THE REJECTION ROWS ARE THE POINT. A gate that reads the wrong argument is
// not merely "too strict" — it is a gate that stops guarding the argument it
// guards. So the rows below pin that the reordered call is still rejected
// when the separator really is inadmissible, and that the diagnostic now
// POINTS AT THE SEPARATOR (its location is asserted, not just its text): the
// pre-fix gate pointed at the ITEMS argument.
//
// WHERE THE LOCATION LIVES. The harness captures the driver's rendered
// diagnostic TEXT, so the `<path>:<line>:<col>: ` prefix is part of the
// message and the structured `Diagnostic::location` fields are unset (they
// are the front end's, not the driver's). The location is therefore asserted
// as the prefix the driver printed.

#include <cstddef>
#include <string>

#include "support/Diagnostic.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// The parts every row joins: three elements, so a separator that IS used is
// observable in the value.
const char* kParts = "    var parts: Array<string> := [\"a\", \"b\", \"c\"];\n";

// True iff SOME error diagnostic's message contains `needle`.
//
// (The helper is `bool`-shaped on purpose: a rejection row that pins the
// diagnostic's TEXT — not merely `r.ran == false` — is what makes the row
// distinguish a genuine front-end rejection from a crash, a signal, a
// timeout or an ASan report; see scripts/check_ambiguous_ran_false.py.)
bool errorWith(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff SOME error diagnostic's message contains BOTH `needle` and
// `loc` — the text pin and the location pin in one row.
bool errorWithAt(const JitResult& r, const std::string& needle,
                 const std::string& loc) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos &&
            d.message.find(loc) != std::string::npos)
            return true;
    return false;
}

// The `<line>:<col>: ` location prefix the driver prints for `needle` in
// `src` — the shape it renders in front of every diagnostic message.
std::string locationOf(const std::string& src, const std::string& needle) {
    const std::size_t pos = src.find(needle);
    if (pos == std::string::npos) return "";
    std::size_t line = 1;
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < pos; ++i)
        if (src[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    return ":" + std::to_string(line) + ":" +
           std::to_string(pos - lineStart + 1) + ": ";
}

}  // namespace

// ── THE REGRESSION ROW ─────────────────────────────────────────────────────
// The reordered named call: the separator is the SECOND written argument, and
// it binds to the parameter it NAMES.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           reordered_named_args_bind_by_parameter_name_22_5) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(items "
                                  ":= parts, separator := \",\"));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("a,b,c"));
}

// ── CONTROLS — the two spellings that were already accepted ───────────────

// The named call in DECLARATION order (the identity binding).
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           named_args_in_declaration_order_unchanged_9_6) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(separator "
                                  ":= \",\", items := parts));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("a,b,c"));
}

// The positional call.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           positional_call_unchanged_9_6) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(\",\", "
                                  "parts));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("a,b,c"));
}

// The PascalCase alias of the same type keyword (§3.4) reaches the same gate
// through the same registry row.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           pascalcase_head_reordered_named_call_accepted_9_6) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(String.Join(items "
                                  ":= parts, separator := \",\"));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), std::string("a,b,c"));
}

// ── THE GATE STILL GUARDS THE SEPARATOR ───────────────────────────────────
// A gate that reads the wrong argument stops guarding the argument it
// guards, so the rejection rows are pinned too — with the diagnostic's
// LOCATION asserted, not only its text.

// A `char` separator in the reordered call: §9.6 declares the separator a
// `string` and there is no `char` overload, so the call is rejected — and the
// diagnostic must POINT AT THE SEPARATOR, not at the items argument.
//
// Pre-fix, this program was rejected with the SAME sentence but pointing at
// `parts` (the first written argument): the gate had looked at the wrong
// argument and named the wrong parameter.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           reordered_named_call_rejects_char_separator_at_the_separator_5_1) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(items "
                                  ":= parts, separator := ','));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(errorWithAt(
        r, "the separator argument to `string.Join` has a type that is not "
           "implicitly convertible to the target type (§5.1)",
        locationOf(src, "','")));
}

// A `null` separator in the reordered call: §9.6 declares the separator
// NON-nullable, and §7 rejects a nullable value at a non-nullable target.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           reordered_named_call_rejects_null_separator_7) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(items "
                                  ":= parts, separator := null));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        errorWithAt(r, "the separator argument to `string.Join` is nullable "
                       "but the target type is non-nullable (§7)",
                    locationOf(src, "null")));
}

// A name that is not a parameter of `string.Join` — §22.5's own rejection,
// reported by the §22.5 binder (not by this gate).
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           unknown_parameter_name_still_rejected_22_5) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(items "
                                  ":= parts, sep := \",\"));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(errorWith(r, "has no parameter named `sep` (§22.5)"));
}

// A positional argument after a named one — §22.5's ordering rule.
BEZEL_TEST(sema_string_join_named_arg_binding_7191,
           positional_argument_after_named_argument_still_rejected_22_5) {
    const std::string src = prog(std::string(kParts) +
                                  "    Console.WriteLine(string.Join(items "
                                  ":= parts, \",\"));\n"
                                  "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        errorWith(r, "a positional argument may not follow a named argument "
                     "(§22.5"));
}
