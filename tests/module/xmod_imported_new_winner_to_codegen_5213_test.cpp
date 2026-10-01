// Regression rows for #5213 — `new ImportedT(...)` against an imported class
// with SEVERAL constructors must RUN the constructor body of the §22.6 winner
// sema selected (spec §22.5 / §22.6 / §2.5, v0.313). Before the fix sema
// diagnosed the imported overload set but did not publish the winner, and
// codegen's `ctorForArgs` re-selected on its own: `new G<string>("s", 2)` on an
// imported generic class ran NO constructor body with no diagnostic, and
// named-argument calls degraded to a default-initialized instance (#2658
// warning). Oracle: exact stdout of the constructor bodies.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct Tmp5213 {
    std::string path;
    explicit Tmp5213(std::string p) : path(std::move(p)) {}
    ~Tmp5213() { std::remove(path.c_str()); }
};

void write5213(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const char* kLib5213 =
    "module PLib5213;\n"
    "public class Counted\n"
    "  private static readonly _n: AtomicInteger<int> := new AtomicInteger<int>(0);\n"
    "  public static method Live(): int => _n.Value;\n"
    "  public constructor() _n.Value := _n.Value + 1; end constructor;\n"
    "  destructor _n.Value := _n.Value - 1; end destructor;\n"
    "end Counted;\n"
    "public class H\n"
    "  private owned _c: Counted;\n"
    "  public constructor(a: int) _c := new Counted(); end constructor;\n"
    "  public constructor(a: int, b: int) _c := new Counted(); end constructor;\n"
    "end H;\n"
    "public class G<T>\n"
    "  public constructor(a: T) Console.WriteLine(\"G T\"); end constructor;\n"
    "  public constructor(a: T, b: int) Console.WriteLine(\"G T,int\"); end constructor;\n"
    "end G;\n"
    "public class C3\n"
    "  public constructor(x: int, y: int) Console.WriteLine(\"C3 int,int \" + x.ToString() + \" \" + y.ToString()); end constructor;\n"
    "  public constructor(y: string, x: int) Console.WriteLine(\"C3 string,int \" + y + \" \" + x.ToString()); end constructor;\n"
    "end C3;\n";

// Returns {-2 lib failed, -1 app rejected, else exit code}, stdout+stderr.
std::pair<int, std::string> run5213(const std::string& mainBody) {
    auto dir = wtScratchDir("wt-5213-module");
    std::string stem = unique_stem("bz5213_");
    Tmp5213 lSrc((dir / (stem + "_l.bzl")).string());
    Tmp5213 lObj((dir / (stem + "_l.o")).string());
    write5213(lSrc.path, kLib5213);
    {
        bezel::Driver d;
        if (!d.compileObject(lSrc.path, lObj.path).success) return {-2, ""};
    }
    Tmp5213 aSrc((dir / (stem + "_a.bzl")).string());
    Tmp5213 aExe((dir / (stem + "_a")).string());
    write5213(aSrc.path,
              "module PApp5213; import PLib5213;\n"
              "class P public static method Main(): int\n" + mainBody +
              "\nreturn 0;\nend Main; end P;\n");
    bezel::Driver driver;
    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, {dep})
             .success)
        return {-1, ""};
    std::string out;
    FILE* p = popen((aExe.path + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

}  // namespace

BEZEL_TEST(xmod_imported_new_winner_5213,
           generic_two_arg_ctor_body_runs_spec_22_6) {
    auto [rc, out] = run5213(
        "var g2 := new G<string>(\"s\", 2);\n"
        "var g3 := new G<int>(3, 4);\n"
        "var g4 := new G<string>(b := 5, a := \"n\");\n"
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("G T,int\nG T,int\nG T,int\nend\n"));
}

BEZEL_TEST(xmod_imported_new_winner_5213,
           generic_one_arg_ctor_body_runs_spec_22_6) {
    auto [rc, out] = run5213(
        "var g := new G<string>(\"s\");\n"
        "var h := new G<int>(3);\n"
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("G T\nG T\nend\n"));
}

BEZEL_TEST(xmod_imported_new_winner_5213,
           named_args_both_orders_first_overload_spec_22_5) {
    auto [rc, out] = run5213(
        "var a := new C3(x := 1, y := 2);\n"
        "var b := new C3(y := 3, x := 4);\n"
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("C3 int,int 1 2\nC3 int,int 4 3\nend\n"));
}

BEZEL_TEST(xmod_imported_new_winner_5213,
           named_args_pick_second_overload_spec_22_6) {
    auto [rc, out] = run5213(
        "var a := new C3(x := 1, y := \"s\");\n"
        "var b := new C3(y := \"t\", x := 2);\n"
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out,
                    std::string("C3 string,int s 1\nC3 string,int t 2\nend\n"));
}

// Leak oracle: the chosen constructor's body assigns an owned field; 200
// constructions must leave Counted.Live() == 0 once each holder is disposed.
BEZEL_TEST(xmod_imported_new_winner_5213,
           chosen_ctor_initialises_owned_field_no_leak_x200) {
    auto [rc, out] = run5213(
        "for var i in 0..<200 do\n"
        "  using var h := new H(b := 2, a := i);\n"
        "end;\n"
        "Console.WriteLine(Counted.Live().ToString());");
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("0\n"));
}

}  // namespace bezel::module
