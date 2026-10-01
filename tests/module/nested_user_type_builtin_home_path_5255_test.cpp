// Module — #5255 (follow-up to #5241; spec §2.2, v0.313; §11.6 nested types):
// a NESTED user type whose enclosing-type path spells a stdlib type's home
// module path (`System > Diagnostics > Stopwatch`) must stay the USER type
// across a module boundary. #5241 exported an alias-qualified builtin as
// `<home>.<Name>`, byte-identical to that nested user type's qualified name, so
// the importer bound the stdlib type instead. The builtin spelling is now
// '@'-marked, which no user type path can produce.
//
// Oracle: exact stdout + exit code of the linked importer program.

#include "driver/Driver.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult {
    bool built = false;
    int exitCode = -1;
    std::string out;
};

// Compile libSrc to an object, compile mainSrc against it, link, run capturing
// stdout.
RunResult buildAndRun(const std::string& libSrc, const std::string& mainSrc,
                      const std::string& tag) {
    RunResult res;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_5255_" + tag + "_" + unique_stem("");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return res;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dl;
    dl.metadataPath = lObj.path;
    dl.objectPath = lObj.path;
    deps.push_back(dl);
    bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rm.success) return res;
    res.built = true;

    FILE* p = popen(mExe.path.c_str(), "r");
    if (!p) return res;
    std::array<char, 256> buf;
    while (std::fgets(buf.data(), buf.size(), p)) res.out += buf.data();
    int status = pclose(p);
    if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
    return res;
}


const char* kLibNested =
    "module LibN; "
    "public class System "
    "  public class Diagnostics "
    "    public class Stopwatch "
    "      private _v: int; "
    "      public constructor(v: int) _v := v; end constructor; "
    "      public method Value(): int => _v; "
    "      public property IsRunning: int => 777; "
    "    end Stopwatch; "
    "  end Diagnostics; "
    "end System; "
    "public class Maker "
    "  public static method Mk(): owned System.Diagnostics.Stopwatch "
    "    return new System.Diagnostics.Stopwatch(9); "
    "  end Mk; "
    "end Maker;";

const char* kLibReal =
    "module LibR; "
    "import System.Diagnostics as D; "
    "public class Holder "
    "  public static method Make(): owned D.Stopwatch "
    "    return D.Stopwatch.StartNew(); "
    "  end Make; "
    "end Holder;";

const char* kLibStack =
    "module LibS; "
    "public class System "
    "  public class Collections "
    "    public class Generic "
    "      public class Stack "
    "        private _v: int; "
    "        public constructor(v: int) _v := v; end constructor; "
    "        public method Top(): int => _v; "
    "      end Stack; "
    "    end Generic; "
    "  end Collections; "
    "end System; "
    "public class Maker "
    "  public static method Mk(): owned System.Collections.Generic.Stack "
    "    return new System.Collections.Generic.Stack(5); "
    "  end Mk; "
    "end Maker;";

}  // namespace

// R1: the issue repro.
BEZEL_TEST(nested_user_type_builtin_home_path_5255,
          nested_user_stopwatch_stays_user_type_spec_2_2) {
    RunResult r = buildAndRun(
        kLibNested,
        "module App; import LibN; "
        "class Program public static method Main(): int "
        "  using var a := Maker.Mk(); "
        "  Console.WriteLine(a.Value()); "
        "  return 0; "
        "end Main; end Program;",
        "r1");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("9\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// R2: a member name shared with the stdlib type binds the USER member.
BEZEL_TEST(nested_user_type_builtin_home_path_5255,
          nested_user_stopwatch_shared_member_binds_user_spec_2_2) {
    RunResult r = buildAndRun(
        kLibNested,
        "module App; import LibN; "
        "class Program public static method Main(): int "
        "  using var a := Maker.Mk(); "
        "  Console.WriteLine(a.IsRunning); "
        "  return 0; "
        "end Main; end Program;",
        "r2");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("777\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// R3: control -- the alias-qualified real builtin still loads as the stdlib.
BEZEL_TEST(nested_user_type_builtin_home_path_5255,
          alias_qualified_real_builtin_still_stdlib_spec_2_2) {
    RunResult r = buildAndRun(
        kLibReal,
        "module App; import LibR; "
        "class Program public static method Main(): int "
        "  using var s := Holder.Make(); "
        "  Console.WriteLine(s.IsRunning); "
        "  return 0; "
        "end Main; end Program;",
        "r3");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("True\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// R4: Stack's home path is System.Collections.Generic.
BEZEL_TEST(nested_user_type_builtin_home_path_5255,
          nested_user_stack_stays_user_type_spec_2_2) {
    RunResult r = buildAndRun(
        kLibStack,
        "module App; import LibS; "
        "class Program public static method Main(): int "
        "  using var a := Maker.Mk(); "
        "  Console.WriteLine(a.Top()); "
        "  return 0; "
        "end Main; end Program;",
        "r4");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("5\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace bezel::module
