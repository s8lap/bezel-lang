// fix #5352 (stdlib §7.4 / §3, v0.313):
// Directory.GetFiles, Directory.GetDirectories, and Environment.Variables
// had no result TypeRef in sema — ctx.exprType(call) was null for these
// calls. The static-call dispatch recognized the callee (colName was
// "Directory" / "Environment") but no arm stamped the result type, so
// inferred locals degraded to zero-initialized slots with the #2434
// internal warning.
//
// THE DEFECT. The colName lambda in ExprTypecheck_Calls.cpp correctly
// identifies these as static calls on the Directory/Environment builtin
// classes, but the per-class arms only typed Directory.Exists (bool).
// GetFiles/GetDirectories/Variables had no typing arm, so setExprType
// was never called.
//
// THE FIX. Adds three arms:
//   Directory.GetFiles(path)   → owned Array<string>  (§7.4)
//   Directory.GetDirectories(path) → owned Array<string>  (§7.4)
//   Environment.Variables()    → owned Dictionary<string, string> (§3)
//
// Rows:
//   R1  Directory.GetFiles("/tmp").Count — types as Array<string>, Count access works.
//   R2  Directory.GetDirectories("/tmp").Count — same shape.
//   R3  Environment.Variables().Count — Dictionary<string,string> has Count.
//   R4  var f := Directory.GetFiles("/tmp"); var n := f.Count — type inferred, n is int.
//   R5  var d := Directory.GetDirectories("/tmp"); var n := d.Count — same.
//   R6  var e := Environment.Variables(); var n := e.Count — same.
//
// All compile and run (codegen already existed for these methods).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── R1 — Directory.GetFiles("/tmp").Count compiles and runs.
BEZEL_TEST(sema_directory_getfiles_typing_5352,
           directory_getfiles_count_accepted) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var files := Directory.GetFiles(\"/tmp\"); "
        "    Console.WriteLine(files.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Count is a non-negative integer; confirm something was printed.
    BEZEL_EXPECT_TRUE(!r.stdoutText.empty());
}

// ── R2 — Directory.GetDirectories("/tmp").Count compiles and runs.
BEZEL_TEST(sema_directory_getdirectories_typing_5352,
           directory_getdirectories_count_accepted) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var dirs := Directory.GetDirectories(\"/tmp\"); "
        "    Console.WriteLine(dirs.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(!r.stdoutText.empty());
}

// ── R3 — Environment.Variables().Count compiles and runs.
BEZEL_TEST(sema_environment_variables_typing_5352,
           environment_variables_count_accepted) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var env := Environment.Variables(); "
        "    Console.WriteLine(env.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(!r.stdoutText.empty());
}

// ── R4 — local from Directory.GetFiles has inferred type Array<string>.
BEZEL_TEST(sema_directory_getfiles_typing_5352,
           directory_getfiles_local_inferred_array) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var f := Directory.GetFiles(\"/tmp\"); "
        "    var n := f.Count; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Without the fix, the local f has no type, f.Count is untypable,
    // and this program does not compile. With the fix, it compiles and
    // prints the actual count (at least 0).
    BEZEL_EXPECT_TRUE(r.stdoutText.find_first_of("0123456789") != std::string::npos);
}

// ── R5 — local from Directory.GetDirectories has inferred type Array<string>.
BEZEL_TEST(sema_directory_getdirectories_typing_5352,
           directory_getdirectories_local_inferred_array) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := Directory.GetDirectories(\"/tmp\"); "
        "    var n := d.Count; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find_first_of("0123456789") != std::string::npos);
}

// ── R6 — local from Environment.Variables has inferred type Dictionary<string,string>.
BEZEL_TEST(sema_environment_variables_typing_5352,
           environment_variables_local_inferred_dict) {
    auto r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var e := Environment.Variables(); "
        "    var n := e.Count; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find_first_of("0123456789") != std::string::npos);
}
