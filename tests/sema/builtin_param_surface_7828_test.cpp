// Regression tests for issue #7828 — the `File` / `Directory` §22.5 NAME
// SURFACE of the ONE built-in parameter table
// (`src/common/BuiltinRegistry.cpp`'s `kBuiltinParams`).
//
// stdlib §7.3 declares `File.Exists(path: string)`, `File.Copy(source:
// string, destination: string, overwrite: bool)` and `File.WriteAllText(path:
// string, contents: string)`; stdlib §7.4 declares `Directory.Exists(path:
// string)` and `Directory.Delete(path: string, recursive: bool := false)`.
// The table carries those DECLARED NAMES, and the heads are in the §22.5
// name table, so a named argument binds the parameter it NAMES and a name
// the member does NOT declare is refused with the §22.5 diagnostic.
//
// The defect the issue reports — "the `File.*` / `Directory.*` rows carry NO
// parameter names, so a named argument binds by POSITION" — is a RESIDUAL of
// f0badd00 (fix #7982), which put the declared names into the table and the
// heads into `kBuiltinNamedArgTableHeads`.  The rows below FAIL on the
// pre-#7982 tree (a named argument is refused there, and the reordered
// named calls bind by position) and PASS on this one.
//
// The ORACLE: the ratchet `scripts/check_builtin_declared_params.py` now
// COMPARES the `names` column — the §22.5 name surface — with the
// document's own declarations, so a name that drifts out of the document is
// a FAILING ratchet and not a silent refusal.
//
// Oracle: runAot.  The accept rows assert the program's OUTPUT; the reject
// rows assert the SPECIFIC diagnostic (a `Severity::Error` match).
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A program with a `Main` and the auto-imported modules the rows need.
std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + imports +
           "class P\n"
           "  public static method Main(): int\n" + body +
           "\n    return 0;\n"
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

// stdlib §7.3 `Exists(path: string)` + spec §22.5: the named argument is
// ADMITTED — the compiler honours the name.  (The BINDING is observed by
// the rows below: a name the member does not declare is refused, and a
// REORDERED named call binds the parameter it names.)
BEZEL_TEST(builtin_param_surface_7828, file_exists_named_argument_is_admitted) {
    auto r = runAot(prog("",
        "    Console.WriteLine(File.Exists(path := \"@DATA@/bzt7828.txt\"));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// The name the member does NOT declare is refused, and the diagnostic NAMES
// it (spec §22.5).
BEZEL_TEST(builtin_param_surface_7828, file_exists_unknown_name_is_refused) {
    auto r = runAot(prog("", "    Console.WriteLine(File.Exists(pth := \"x\"));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "File.Exists"));
    BEZEL_EXPECT_TRUE(hasError(r, "has no parameter named `pth`"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§22.5)"));
}

// The POSITIONAL spelling still works.
BEZEL_TEST(builtin_param_surface_7828, file_exists_positional_still_works) {
    auto r = runAot(prog("", "    Console.WriteLine(File.Exists(\"@DATA@/x.txt\"));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// stdlib §7.4 `Directory.Exists(path: string)`: the same for the `Directory`
// head.
BEZEL_TEST(builtin_param_surface_7828, directory_exists_named_binds_by_name) {
    auto r = runAot(prog("",
        "    Console.WriteLine(Directory.Exists(path := \"@DATA@/bzt7828d\"));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// stdlib §7.4 `Directory.Delete(path: string, recursive: bool := false)`
// + spec §22.5: the names bind the parameters they NAME, in any order —
// `recursive` binds the `recursive` parameter and `path` the `path` one.
// Read by POSITION the call would pass `true` as the `path`, and the
// directory would NOT be deleted.
BEZEL_TEST(builtin_param_surface_7828, directory_delete_reordered_named_binds_by_name) {
    const std::string body =
        "    Directory.CreateDirectory(path := \"@DATA@/bzt7828d2\");\n"
        "    Directory.Delete(recursive := true, path := \"@DATA@/bzt7828d2\");\n"
        "    Console.WriteLine(Directory.Exists(path := \"@DATA@/bzt7828d2\"));\n";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7828"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\n");
}

// stdlib §7.3 `Copy(source: string, destination: string, overwrite: bool)`
// + spec §22.5: the names bind the parameters they NAME — SRC is copied to
// DST.  Read by position the call copied DST over SRC and DESTROYED THE
// SOURCE.
BEZEL_TEST(builtin_param_surface_7828, file_copy_named_binds_source_to_source) {
    const std::string body =
        "    var src := \"@DATA@/bzt7828src.txt\";\n"
        "    var dst := \"@DATA@/bzt7828dst.txt\";\n"
        "    File.WriteAllText(path := src, contents := \"SRC-CONTENT\");\n"
        "    File.WriteAllText(path := dst, contents := \"DST-CONTENT\");\n"
        "    File.Copy(destination := dst, source := src, overwrite := true);\n"
        "    Console.WriteLine(File.ReadAllText(path := src));\n"
        "    Console.WriteLine(File.ReadAllText(path := dst));\n";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7828"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "SRC-CONTENT\nSRC-CONTENT\n");
}
