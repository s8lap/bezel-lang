// Regression tests for issue #7933 — `File.Exists("a", "b")`, `Directory.Exists()`
// and the other static members of `File`, `Directory` and `Environment`
// compiled at an arity the standard library declares no form for, with only
// the #3079 "failed to lower ... emits nothing" warning (the #7864 `Path`
// defect, generalized).
//
// stdlib §7.3 (v0.350) declares `File.Exists(path: string)`, `Copy(source,
// destination, overwrite: bool)`, ...; §7.4 declares `Directory.Exists(path)`,
// `Delete(path, recursive := false)`, ...; §2.4 declares `Environment.Get(name)`,
// `Set(name, value)`, `Variables()`, `Exit(exitCode)`. A call at a count no
// declared form takes has no candidate: a compile error (§22.6). The REJECT
// rows assert the specific diagnostic; the ACCEPT rows run every declared
// arity; the CONTROL rows pin that a user class of the same name keeps its own
// arity.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + decls +
           "class P\n"
           "  public static method Main(): int\n" + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

void expectArityRejected(const std::string& body, const std::string& callee,
                         int supplied, const std::string& section,
                         const std::string& declared) {
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string want = "the call to `" + callee + "` supplies " +
                             std::to_string(supplied) + " argument(s)";
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(want) != std::string::npos &&
            d.message.find("stdlib §" + section) != std::string::npos &&
            d.message.find("with " + declared + " argument(s)") !=
                std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}

}  // namespace

BEZEL_TEST(builtin_fs_env_arity_7933, file_exists_two_arguments_is_rejected) {
    // per stdlib §7.3 (v0.350) `Exists(path: string): bool`
    expectArityRejected("    Console.WriteLine(File.Exists(\"a\", \"b\"));",
                        "File.Exists", 2, "7.3", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, directory_exists_no_arguments_is_rejected) {
    // per stdlib §7.4 (v0.350) `Exists(path: string): bool`
    expectArityRejected("    Console.WriteLine(Directory.Exists());",
                        "Directory.Exists", 0, "7.4", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, file_copy_two_arguments_is_rejected) {
    // per stdlib §7.3 `Copy(source, destination, overwrite: bool)`
    expectArityRejected("    File.Copy(\"a\", \"b\");", "File.Copy", 2, "7.3",
                        "exactly 3");
}

BEZEL_TEST(builtin_fs_env_arity_7933, file_move_three_arguments_is_rejected) {
    // per stdlib §7.3 `Move(source, destination)`
    expectArityRejected("    File.Move(\"a\", \"b\", \"c\");", "File.Move", 3,
                        "7.3", "exactly 2");
}

BEZEL_TEST(builtin_fs_env_arity_7933, file_writealltext_one_argument_is_rejected) {
    // per stdlib §7.3 `WriteAllText(path, contents)`
    expectArityRejected("    File.WriteAllText(\"a\");", "File.WriteAllText", 1,
                        "7.3", "exactly 2");
}

BEZEL_TEST(builtin_fs_env_arity_7933, file_readalllines_no_arguments_is_rejected) {
    // per stdlib §7.3 `ReadAllLines(path)`
    expectArityRejected("    using var l := File.ReadAllLines();",
                        "File.ReadAllLines", 0, "7.3", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, file_readalltextasync_three_arguments_is_rejected) {
    // per stdlib §7.3 `ReadAllTextAsync(path, token := ...)`: one or two
    expectArityRejected("    var t := File.ReadAllTextAsync(\"a\", \"b\", \"c\");",
                        "File.ReadAllTextAsync", 3, "7.3", "1 or 2");
}

BEZEL_TEST(builtin_fs_env_arity_7933, directory_delete_three_arguments_is_rejected) {
    // per stdlib §7.4 `Delete(path, recursive: bool := false)`: one or two
    expectArityRejected("    Directory.Delete(\"a\", true, 1);",
                        "Directory.Delete", 3, "7.4", "1 or 2");
}

BEZEL_TEST(builtin_fs_env_arity_7933, directory_getfiles_two_arguments_is_rejected) {
    // per stdlib §7.4 `GetFiles(path)`
    expectArityRejected("    using var f := Directory.GetFiles(\"a\", \"*\");",
                        "Directory.GetFiles", 2, "7.4", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_get_no_arguments_is_rejected) {
    // per stdlib §2.4 (v0.350) `Get(name: string): string?`
    expectArityRejected("    var v := Environment.Get();", "Environment.Get", 0,
                        "2.4", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_set_one_argument_is_rejected) {
    // per stdlib §2.4 `Set(name: string, value: string?)`
    expectArityRejected("    Environment.Set(\"A\");", "Environment.Set", 1, "2.4",
                        "exactly 2");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_getint_two_arguments_is_rejected) {
    // per stdlib §2.4 `GetInt(name: string): int?`
    expectArityRejected("    var v := Environment.GetInt(\"A\", \"B\");",
                        "Environment.GetInt", 2, "2.4", "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_variables_one_argument_is_rejected) {
    // per stdlib §2.4 `Variables(): owned Dictionary<string, string>`
    expectArityRejected("    using var v := Environment.Variables(1);",
                        "Environment.Variables", 1, "2.4", "exactly 0");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_exit_no_arguments_is_rejected) {
    // per stdlib §2.4 `Exit(exitCode: int)`
    expectArityRejected("    Environment.Exit();", "Environment.Exit", 0, "2.4",
                        "exactly 1");
}

BEZEL_TEST(builtin_fs_env_arity_7933, declared_file_and_directory_arities_run) {
    // per stdlib §7.3 / §7.4: every declared arity of every File / Directory
    // method; each value is printed, so a miscompiled arity is visible.
    const std::string body =
        "    var d := \"@DATA@/d\";\n"
        "    Directory.CreateDirectory(d);\n"
        "    Console.WriteLine(Directory.Exists(d));\n"
        "    var a := Path.Combine(d, \"a.txt\");\n"
        "    var b := Path.Combine(d, \"b.txt\");\n"
        "    var c := Path.Combine(d, \"c.txt\");\n"
        "    File.WriteAllText(a, \"hi\");\n"
        "    Console.WriteLine(File.ReadAllText(a));\n"
        "    Console.WriteLine(File.Exists(a));\n"
        "    File.Copy(a, b, true);\n"
        "    File.Move(b, c);\n"
        "    Console.WriteLine(File.Exists(b));\n"
        "    using var lines := new Array<string>();\n"
        "    lines.Add(\"x\"); lines.Add(\"y\");\n"
        "    File.WriteAllLines(a, lines);\n"
        "    using var back := File.ReadAllLines(a);\n"
        "    Console.WriteLine(back.Count);\n"
        "    using var bytes := new Array<byte>();\n"
        "    bytes.Add(byte(7));\n"
        "    File.WriteAllBytes(a, bytes);\n"
        "    using var bb := File.ReadAllBytes(a);\n"
        "    Console.WriteLine(bb.Count);\n"
        "    using var fs := Directory.GetFiles(d);\n"
        "    Console.WriteLine(fs.Count);\n"
        "    using var ds := Directory.GetDirectories(d);\n"
        "    Console.WriteLine(ds.Count);\n"
        "    File.Delete(a);\n"
        "    File.Delete(c);\n"
        "    Directory.Delete(d);\n"
        "    Console.WriteLine(Directory.Exists(d));\n"
        "    Directory.CreateDirectory(d);\n"
        "    Directory.Delete(d, true);\n"
        "    Console.WriteLine(Directory.Exists(d));";
    auto r = runAot(bezel::test::spliceDataPaths(prog("", body), "bzt7933"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "True\nhi\nTrue\nFalse\n2\n1\n2\n0\nFalse\nFalse\n");
}

BEZEL_TEST(builtin_fs_env_arity_7933, declared_environment_arities_run) {
    // per stdlib §2.4: Set takes 2, Get takes 1 (GetInt / GetBool / Variables
    // are declared but their lowering is not implemented at ANY arity, so they
    // have no accept row here: see the reject rows for their declared counts)
    const std::string body =
        "    Environment.Set(\"BZ7933_N\", \"42\");\n"
        "    Console.WriteLine(Environment.Get(\"BZ7933_N\"));";
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

BEZEL_TEST(builtin_fs_env_arity_7933, environment_exit_one_argument_runs) {
    // per stdlib §2.4 `Exit(exitCode: int)`: the one declared arity exits
    auto r = runAot(prog("", "    Environment.Exit(7);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(builtin_fs_env_arity_7933, user_class_named_file_keeps_its_own_arity) {
    // control: a user class `File` / `Directory` / `Environment` is bound by
    // the ordinary user-method machinery, not by the standard library's table.
    const std::string decls =
        "public static class File\n"
        "  public static method Exists(a: int, b: int): int\n"
        "    return a + b;\n"
        "  end Exists;\n"
        "end File;\n"
        "public static class Directory\n"
        "  public static method Exists(): int\n"
        "    return 5;\n"
        "  end Exists;\n"
        "end Directory;\n"
        "public static class Environment\n"
        "  public static method Get(a: int, b: int, c: int): int\n"
        "    return a + b + c;\n"
        "  end Get;\n"
        "end Environment;\n";
    auto r = runAot(prog(decls,
        "    Console.WriteLine(File.Exists(1, 2));\n"
        "    Console.WriteLine(Directory.Exists());\n"
        "    Console.WriteLine(Environment.Get(1, 2, 3));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n5\n6\n");
}
