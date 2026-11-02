// Regression tests for issue #7995 — a NAMED argument at a `Path` member call
// keeps the §7 nullability gate's diagnostic (spec §7 / §7.3, v0.350), and
// the named argument binds the parameter it NAMES (spec §22.5, v0.350).
//
// stdlib §7.4 (v0.350) declares
//   `public static class Path`
//     `public static method Combine(a: string, b: string): string;`
//     `public static method GetFileName(path: string): string;`
//     `public static method GetExtension(path: string): string;`
//     `public static method GetFileNameWithoutExtension(path: string): string;`
//     `public static method GetDirectoryName(path: string): string;`
//     `public static method GetFullPath(path: string): string;`
//   — every parameter is a NON-nullable `string`, and §1 of the standard
//   library makes the parameter names API surface ("parameter names are API
//   surface (named arguments, spec §22.5)").
//
// §7.3: "Assigning `null` to a non-nullable target is a compile error." The
// POSITIONAL spelling `Path.Combine(null, "x")` is refused; the NAMED
// spelling `Path.Combine(a := null, b := "x")` must be refused TOO, and is
// refused AT THE POSITION THE NAME BINDS TO. Before the fix the named
// spelling compiled clean and the program SEGFAULTED (exit 139): the `null`
// reached the runtime's `strlen` unchecked.
//
// §22.5: "Each supplied argument binds exactly one parameter ..." — so
// `Path.Combine(b := "B", a := "A")` combines A with B ("A/B"), not B with A.
//
// Oracle: runAot. Reject rows assert the specific §7 diagnostic; accept rows
// assert the program's OUTPUT.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\nimport System;\nimport System.IO;\n" + imports +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

bool hasError(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The refusal must be a COMPILE-TIME refusal: the program must never have
// been built and run. Before the fix the NAMED spelling compiled clean, the
// `null` reached the runtime's `strlen`, and the program was killed by
// SIGSEGV — the harness reports that as "terminated by signal 11" with
// `ran == false`, so the `ran` check alone cannot tell a refusal from the
// crash. This arm tells them apart.
bool crashedAtRuntime(const bezel::test::JitResult& r) {
    return hasError(r, "terminated by signal");
}

}  // namespace

// ---------------------------------------------------------------------------
// §7 / §7.3: the nullability gate keeps its diagnostic for the NAMED
// spelling — checked AT THE POSITION THE NAME BINDS TO.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_path_named_arg_7995, path_combine_named_null_is_rejected) {
    // per stdlib §7.4 `Combine(a: string, b: string)` (NON-nullable
    // parameters) + §7 "Assigning `null` to a non-nullable target is a compile
    // error": the NAMED spelling is refused exactly as the positional one is.
    // Before the fix this compiled clean and the program SEGFAULTED (139).
    auto r = runAot(prog("", "    var p := Path.Combine(a := null, b := \"x\");\n"
                             "    Console.WriteLine(p);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.Combine` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

BEZEL_TEST(builtin_path_named_arg_7995, path_combine_named_reordered_null_is_rejected) {
    // per stdlib §7.4 + §7 / §7.3 + §22.5: the `null` argument binds the `a`
    // parameter — the gate reads the POSITION THE NAME BINDS TO, not the
    // argument's index (the index here is the `b` one).
    auto r = runAot(prog("", "    var p := Path.Combine(b := \"x\", a := null);\n"
                             "    Console.WriteLine(p);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.Combine` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

BEZEL_TEST(builtin_path_named_arg_7995, path_getfilename_named_null_is_rejected) {
    // per stdlib §7.4 `GetFileName(path: string)` + §7 / §7.3: the same hole
    // at a ONE-argument `Path` member.
    auto r = runAot(prog("", "    Console.WriteLine(Path.GetFileName(path := null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.GetFileName` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

BEZEL_TEST(builtin_path_named_arg_7995, path_getfullpath_named_null_is_rejected) {
    // per stdlib §7.4 `GetFullPath(path: string)` + §7 / §7.3: the gate
    // covers every `Path` member whose parameter is declared non-nullable,
    // not just `Combine`.
    auto r = runAot(prog("", "    Console.WriteLine(Path.GetFullPath(path := null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.GetFullPath` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

// ---------------------------------------------------------------------------
// Controls: the POSITIONAL spelling and the legal named spelling keep their
// behaviour.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_path_named_arg_7995, path_combine_positional_null_is_rejected) {
    // control: the POSITIONAL spelling was already refused before the fix
    // and stays refused, with the same §7 wording.
    auto r = runAot(prog("", "    var p := Path.Combine(null, \"x\");\n"
                             "    Console.WriteLine(p);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.Combine` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

BEZEL_TEST(builtin_path_named_arg_7995, path_getfilename_positional_null_is_rejected) {
    // control: the POSITIONAL spelling of the one-argument member.
    auto r = runAot(prog("", "    Console.WriteLine(Path.GetFileName(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Path.GetFileName` is nullable but the target type is "
        "non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(crashedAtRuntime(r));
}

BEZEL_TEST(builtin_path_named_arg_7995, path_combine_named_valid_is_accepted) {
    // control: the legal NAMED spelling still compiles and runs.
    auto r = runAot(prog("", "    Console.WriteLine(Path.Combine(a := \"A\", b := \"B\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "A/B\n");
}

BEZEL_TEST(builtin_path_named_arg_7995, path_getfilename_named_valid_is_accepted) {
    // control: the legal NAMED spelling of a one-argument member still
    // compiles and runs.
    auto r = runAot(prog("", "    Console.WriteLine(Path.GetFileName(path := \"/tmp/dir/file.txt\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "file.txt\n");
}

// ---------------------------------------------------------------------------
// §22.5: a named argument binds the parameter it NAMES.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_path_named_arg_7995, path_named_binding_order) {
    // per stdlib §7.4 `Combine(a: string, b: string)` + §22.5: the `a`
    // argument binds the `a` parameter and the `b` argument binds the `b`
    // parameter, in that written order. Bound by POSITION the call would
    // combine B with A and print "B/A".
    auto r = runAot(prog("", "    Console.WriteLine(Path.Combine(b := \"B\", a := \"A\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "A/B\n");
}
