// Regression tests for issue #7982 — a NAMED argument at a `File` /
// `Directory` member call binds to the parameter it NAMES (spec §22.5,
// v0.350), and the §7 nullability gate keeps its diagnostic for that named
// spelling (spec §7 / §7.3, v0.350).
//
// stdlib §7.3 (v0.350) declares
//   `Copy(source: string, destination: string, overwrite: bool)`,
//   `WriteAllText(path: string, contents: string)`, `Exists(path: string)`,
//   ... and stdlib §7.4 (v0.350) declares
//   `Delete(path: string, recursive: bool := false)`, `Exists(path: string)`,
//   ... — §1 of the standard library makes the parameter names API surface
//   ("parameter names are API surface (named arguments, spec §22.5)").
//
// §22.5: "Each supplied argument binds exactly one parameter and each
// parameter is bound by at most one supplied argument ... Every parameter that
// declares no default must be bound, positionally or by name". So
// `Directory.Delete(recursive := true)` — which names `recursive` and leaves
// the required `path` unbound — is a compile error, and
// `File.Copy(destination := DST, source := SRC, overwrite := true)` copies SRC
// to DST.
//
// §7 / §7.3: "Assigning `null` to a non-nullable target is a compile error",
// and §7.3 of the standard library declares `Exists(path: string)` — a
// NON-nullable parameter. The positional spelling `File.Exists(null)` is
// refused; the NAMED spelling `File.Exists(path := null)` must be refused
// too, and is refused AT THE POSITION THE NAME BINDS TO.
//
// Oracle: runAot / runAotModules. Reject rows assert the specific
// diagnostic; accept rows assert the program's OUTPUT.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + imports +
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

}  // namespace

// ---------------------------------------------------------------------------
// §22.5: a named argument binds the parameter it NAMES.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_fs_named_arg_7982, directory_delete_named_without_path_is_rejected) {
    // per stdlib §7.4 `Delete(path: string, recursive: bool := false)` +
    // §22.5: `path` declares no default, so it must be bound.
    auto r = runAot(prog("", "    Directory.Delete(recursive := true);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "Directory.Delete"));
    // The §22.5 refusal, not a lowering failure: the base admitted the call
    // and codegen then failed IR verification.
    BEZEL_EXPECT_TRUE(hasError(r, "§22.5"));
    BEZEL_EXPECT_FALSE(hasError(r, "IR verification failed"));
}

BEZEL_TEST(builtin_fs_named_arg_7982, directory_delete_named_reordered_deletes_the_path) {
    // per stdlib §7.4 + §22.5: `recursive` binds the `recursive` parameter
    // and `path` binds the `path` parameter, in that written order.
    const std::string body =
        "    Directory.CreateDirectory(path := \"@DATA@/bzt7982d\");\n"
        "    File.WriteAllText(path := \"@DATA@/bzt7982d/x.txt\", contents := \"x\");\n"
        "    Directory.Delete(recursive := true, path := \"@DATA@/bzt7982d\");\n"
        "    Console.WriteLine(Directory.Exists(path := \"@DATA@/bzt7982d\"));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7982"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

BEZEL_TEST(builtin_fs_named_arg_7982, file_copy_named_binds_source_to_source) {
    // per stdlib §7.3 `Copy(source: string, destination: string, overwrite:
    // bool)` + §22.5: the named arguments name SOURCE and DESTINATION, so
    // SRC is copied to DST. Read by position the call copied DST over SRC and
    // DESTROYED THE SOURCE.
    const std::string body =
        "    var src := \"@DATA@/bzt7982src.txt\";\n"
        "    var dst := \"@DATA@/bzt7982dst.txt\";\n"
        "    File.WriteAllText(path := src, contents := \"SRC-CONTENT\");\n"
        "    File.WriteAllText(path := dst, contents := \"DST-CONTENT\");\n"
        "    File.Copy(destination := dst, source := src, overwrite := true);\n"
        "    Console.WriteLine(File.ReadAllText(path := src));\n"
        "    Console.WriteLine(File.ReadAllText(path := dst));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7982"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The source is intact and the destination holds the source's content.
    BEZEL_EXPECT_EQ(r.stdoutText, "SRC-CONTENT\nSRC-CONTENT\n");
}

BEZEL_TEST(builtin_fs_named_arg_7982, file_writealltext_named_binds_path_and_contents) {
    // per stdlib §7.3 `WriteAllText(path: string, contents: string)` +
    // §22.5: `contents` names the contents parameter and `path` the path
    // parameter. Read by position the call wrote the PATH as the file's
    // content and the CONTENTS as its path.
    const std::string body =
        "    File.WriteAllText(contents := \"hi\", path := \"@DATA@/bzt7982w.txt\");\n"
        "    Console.WriteLine(File.ReadAllText(path := \"@DATA@/bzt7982w.txt\"));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7982"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "hi\n");
}

BEZEL_TEST(builtin_fs_named_arg_7982, unknown_parameter_name_is_rejected) {
    // per stdlib §7.3 `Exists(path: string)` + §22.5: `nope` is no parameter
    // of the member, so the name can be neither matched nor bound.
    auto r = runAot(prog("", "    Console.WriteLine(File.Exists(nope := \"a\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "File.Exists"));
    BEZEL_EXPECT_TRUE(hasError(r, "has no parameter named `nope`"));
}

// ---------------------------------------------------------------------------
// §7 / §7.3: the nullability gate keeps its diagnostic for the NAMED
// spelling — checked AT THE POSITION THE NAME BINDS TO.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_fs_named_arg_7982, file_exists_named_null_is_rejected) {
    // per stdlib §7.3 `Exists(path: string)` (a NON-nullable parameter) +
    // §7 "Assigning `null` to a non-nullable target is a compile error":
    // the NAMED spelling is refused exactly as the positional one is.
    auto r = runAot(prog("", "    Console.WriteLine(File.Exists(path := null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `File.Exists` is nullable but the target type is "
        "non-nullable (§7)"));
}

BEZEL_TEST(builtin_fs_named_arg_7982, directory_exists_named_null_is_rejected) {
    // per stdlib §7.4 `Exists(path: string)` + §7 / §7.3: the same refusal
    // for the `Directory` head.
    auto r = runAot(prog("", "    Console.WriteLine(Directory.Exists(path := null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Directory.Exists` is nullable but the target type is "
        "non-nullable (§7)"));
}

BEZEL_TEST(builtin_fs_named_arg_7982, directory_delete_named_null_path_is_rejected) {
    // per stdlib §7.4 `Delete(path: string, recursive: bool := false)` +
    // §7 / §7.3 + §22.5: `path` is the NON-nullable parameter, and the named
    // argument binds IT — the gate reads the position the name binds to, not
    // the argument's index (the index here is the `recursive` one).
    auto r = runAot(prog("",
        "    Directory.Delete(recursive := true, path := null);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r,
        "the argument to `Directory.Delete` is nullable but the target type is "
        "non-nullable (§7)"));
}

// ---------------------------------------------------------------------------
// Controls: the admitted spellings keep working.
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_fs_named_arg_7982, named_arguments_in_declaration_order_run) {
    // control: a named argument list already in DECLARATION order binds the
    // identity permutation, so it is admitted and runs.
    const std::string body =
        "    Directory.CreateDirectory(path := \"@DATA@/bzt7982c\");\n"
        "    File.WriteAllText(path := \"@DATA@/bzt7982c/a.txt\", contents := \"hi\");\n"
        "    Console.WriteLine(File.Exists(path := \"@DATA@/bzt7982c/a.txt\"));\n"
        "    Console.WriteLine(File.ReadAllText(path := \"@DATA@/bzt7982c/a.txt\"));\n"
        "    File.Delete(path := \"@DATA@/bzt7982c/a.txt\");\n"
        "    Directory.Delete(path := \"@DATA@/bzt7982c\");\n"
        "    Console.WriteLine(Directory.Exists(path := \"@DATA@/bzt7982c\"));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7982"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nhi\nFalse\n");
}

BEZEL_TEST(builtin_fs_named_arg_7982, positional_calls_are_unchanged) {
    // control: the positional spelling of every touched member keeps its
    // meaning (stdlib §7.3 / §7.4).
    const std::string body =
        "    Directory.CreateDirectory(\"@DATA@/bzt7982p\");\n"
        "    File.WriteAllText(\"@DATA@/bzt7982p/a.txt\", \"hi\");\n"
        "    Console.WriteLine(File.Exists(\"@DATA@/bzt7982p/a.txt\"));\n"
        "    Console.WriteLine(File.ReadAllText(\"@DATA@/bzt7982p/a.txt\"));\n"
        "    File.Copy(\"@DATA@/bzt7982p/a.txt\", \"@DATA@/bzt7982p/b.txt\", true);\n"
        "    Console.WriteLine(File.ReadAllText(\"@DATA@/bzt7982p/b.txt\"));\n"
        "    Directory.Delete(\"@DATA@/bzt7982p\", true);\n"
        "    Console.WriteLine(Directory.Exists(\"@DATA@/bzt7982p\"));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7982"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nhi\nhi\nFalse\n");
}
