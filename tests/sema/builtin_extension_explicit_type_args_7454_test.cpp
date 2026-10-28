// Spec-derived regression test for issue #7454.
//
// Spec §26.5 rule 2 (v0.345) — "If the receiver's type declares no instance
// member of that name, the compiler searches the current module and all
// imported modules ... for extension blocks on `x`'s type, base types, or
// implemented interfaces" — and spec §14.5 "Explicit lists are all-or-nothing"
// (v0.345) — "An explicit type-argument list (`obj.Method<int>(args)`) binds
// the declared type parameters in declaration order and must supply exactly
// as many arguments as the method declares type parameters."
//
// THE DEFECT. stdlib §9.6 declares NO member named `Tag` on the builtin
// `string` head, so §26.5 rule 2 requires the extension blocks to be
// consulted, and the CALLEE is the extension's member — which DOES declare a
// type parameter. But the compiler's builtin-head check (the one added for
// #7173) rejected the explicit list off the RECEIVER'S TYPE, without asking
// whether the member is a §9.6 member at all:
//
//   F5.bzl:8:23: `string.Tag` is given 1 explicit type argument(s), but it
//   declares no type parameters — an explicit type-argument list must supply
//   exactly as many arguments as the method declares type parameters; there
//   are no partial lists (§14.5, §27.6, v0.345)
//
// MEASURED BEFORE THE FIX (on ea7cd9ac), one-line programs:
//
//   extension string Tag<T>(x: T): string
//   s.Tag<string>("s");   -> the program was REJECTED with the sentence
//                             above — the extension declares `T`, so ONE
//                             argument is exactly right;
//   s.Tag<int>(3);        -> the same rejection.
//
// The builtin head answered for a name it does not declare. The mirror of
// the rule the compiler already applies in the CODEGEN direction (the
// #6159 fix): bind by the RESOLVED CALLEE SYMBOL, not by the receiver's
// name.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE — a rejection row's exit code alone proves nothing).
// The rejection rows assert the SPECIFIC diagnostic at a real source
// location. The CONTROL rows are load-bearing: a §9.6 member must KEEP
// refusing an explicit list, INCLUDING when a same-named user extension
// exists (§26.5 rule 1 commits instance resolution by NAME, so the built-in
// answers and the extension is unreachable).
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A module carrying the extension block, and a `Main` that runs `body`.
std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The extension block of the defect: a member that declares its OWN type
// parameter, over the builtin `string` head.
std::string tagExtension() {
    return "extension string\n"
           "  public method Tag<T>(x: T): string => $\"{this}:{x}\";\n"
           "end;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& ext, const std::string& body,
                     const std::string& needle) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The §14.5 rejection of an explicit list on a member that declares no type
// parameter (the `Math` head's own sentence, stdlib §2.2).
const char kNoTypeParams[] =
    "explicit type argument(s), but it declares no type parameters";

// The compiler's own §14.5 check, read off the RESOLVED callee's declared
// type-parameter count.
const char kWrongListLength[] =
    "supplies 2 explicit type argument(s), but `Tag` declares 1 type "
    "parameter(s)";

}  // namespace

// ── the DEFECT: the extension's own member, reached with a list ────────────

BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           extension_member_with_one_type_arg_is_admitted) {
    // per §26.5 rule 2 (v0.345) + §14.5 (v0.345)
    expectStdout(tagExtension(),
                 "var s: string := \"a\";\n"
                 "    var r: string := s.Tag<string>(\"s\");\n"
                 "    Console.WriteLine(r);",
                 "a:s\n");
}

BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           extension_member_with_a_different_type_arg_is_admitted) {
    // per §26.5 rule 2 (v0.345) + §14.5 (v0.345)
    expectStdout(tagExtension(),
                 "var s: string := \"a\";\n"
                 "    var r: string := s.Tag<int>(3);\n"
                 "    Console.WriteLine(r);",
                 "a:3\n");
}

// The extension's member WITHOUT a list still infers its type parameter
// (§14.5's inference phase) — the fix must not disturb it.
BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           extension_member_without_a_list_still_infers) {
    // per §14.5 (v0.345)
    expectStdout(tagExtension(),
                 "var s: string := \"a\";\n"
                 "    var r: string := s.Tag(\"s\");\n"
                 "    Console.WriteLine(r);",
                 "a:s\n");
}

// A list of the WRONG length on the extension's own member is still a
// compile error — §14.5's all-or-nothing rule, read off the RESOLVED
// member's own declared count (one), not off the builtin head's (none).
//
// The row also pins that the BUILT-IN head's own §14.5 diagnostic is NOT
// among the diagnostics: before the fix the builtin head answered for the
// name the extension owns, so BOTH sentences were reported — the builtin
// head's "`string.Tag` is given 2 explicit type argument(s), but it declares
// no type parameters" AND the compiler's own "supplies 2 explicit type
// argument(s), but `Tag` declares 1 type parameter(s)". The program is
// REJECTED on both arms (so the diagnostics are filled on both arms and the
// absence assertion is not vacuous), but only the resolved member's own
// sentence belongs there.
BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           extension_member_with_too_many_type_args_rejected) {
    // per §14.5 (v0.345)
    auto r = runAot(prog(tagExtension(),
                         "var s: string := \"a\";\n"
                         "    var r: string := s.Tag<string, int>(\"s\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kWrongListLength));
    BEZEL_EXPECT_FALSE(hasError(r, "`string.Tag` is given"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// ── CONTROLS: the built-in head still owns the §9.6 members ────────────────

// A §9.6 member declares NO type parameter, so an explicit list on it is a
// compile error — the #7173 rejection, unchanged.
BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           builtin_string_member_with_a_type_arg_still_rejected) {
    // per §14.5 (v0.345) + stdlib §9.6
    expectRejected("",
                   "var s: string := \"a,b\";\n"
                   "    Console.WriteLine(s.Split<int>(','));",
                   "`string.Split` is given 1 " + std::string(kNoTypeParams));
}

// §26.5 rule 1 (v0.345) — "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` ... instance resolution is
// committed and extension blocks are **never consulted** for that name on
// that receiver." `string` DECLARES `Split` (stdlib §9.6), so the built-in
// answers even though a same-named extension exists, and the explicit list
// is still refused. This is the mirror of the defect: the built-in head must
// answer for the names it OWNS, and must NOT answer for the names it does
// not.
BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           builtin_string_member_with_a_same_named_extension_still_rejected) {
    // per §26.5 rule 1 (v0.345) + §14.5 (v0.345) + stdlib §9.6
    expectRejected("extension string\n"
                   "  public method Split(sep: char): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string := \"a,b\";\n"
                   "    Console.WriteLine(s.Split<int>(','));",
                   "`string.Split` is given 1 " + std::string(kNoTypeParams));
}

// The same-named extension on a §9.6 member is still UNREACHABLE — the
// built-in answers, and the program renders the built-in's value.
BEZEL_TEST(sema_builtin_extension_explicit_type_args_7454,
           builtin_string_member_with_a_same_named_extension_is_unreachable) {
    // per §26.5 rule 1 (v0.345) + stdlib §9.6
    expectStdout("extension string\n"
                 "  public method Split(sep: char): string => \"EXT\";\n"
                 "end;\n",
                 "var s: string := \"a,b\";\n"
                 "    var parts: Array<string> := s.Split(',');\n"
                 "    Console.WriteLine(parts.Count);",
                 "2\n");
}
