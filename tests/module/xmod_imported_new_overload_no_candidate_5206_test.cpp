// Regression for #5206 — `new ImportedT(...)` against an imported class with
// SEVERAL constructors must apply §22.5 binding + §22.6 resolution exactly as
// a local class does (spec §22.5 / §22.6 / §2.8.3 rule 4, v0.313).
//
// Before the fix `checkImportedNewExprArgs` returned at `ctors.size() != 1`
// ("imported ctor overloads deferred"), so a call matching NO candidate
// compiled (only the #2658 "degraded to a default-initialized instance"
// warning) and produced a zero object. Rows 1-2 are the issue's repros and
// FAIL WITHOUT the fix; rows 3-5 are accept controls that pass on both sides
// and pin against over-rejection (they RUN the program and check the exit code).

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct Tmp5206 {
    std::string path;
    explicit Tmp5206(std::string p) : path(std::move(p)) {}
    ~Tmp5206() { std::remove(path.c_str()); }
};

void write5206(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const char* kLib5206 =
    "module PLib5206;\n"
    "public class B\n"
    "  public property V: int := 0;\n"
    "  public constructor(a: string) this.V := 1; end constructor;\n"
    "  public constructor(a: string, b: string) this.V := 2; end constructor;\n"
    "end B;\n"
    "public class W\n"
    "  public property V: int := 0;\n"
    "  public constructor(a: long) this.V := 10; end constructor;\n"
    "  public constructor(a: string, b: string) this.V := 20; end constructor;\n"
    "end W;\n"
    "public class D\n"
    "  public property V: int := 0;\n"
    "  public constructor(a: int, b: int := 5) this.V := a + b; end constructor;\n"
    "  public constructor(s: string) this.V := 99; end constructor;\n"
    "end D;\n";

// Returns -2 lib failed, -1 app rejected (stderr captured), else exit code.
int run5206(const std::string& mainBody, std::string* err) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz5206_" + unique_stem("");
    Tmp5206 lSrc((dir / (stem + "_l.bzl")).string());
    Tmp5206 lObj((dir / (stem + "_l.o")).string());
    write5206(lSrc.path, kLib5206);
    {
        bezel::Driver d;
        if (!d.compileObject(lSrc.path, lObj.path).success) return -2;
    }
    Tmp5206 aSrc((dir / (stem + "_a.bzl")).string());
    Tmp5206 aExe((dir / (stem + "_a")).string());
    Tmp5206 aErr((dir / (stem + "_a.err")).string());
    write5206(aSrc.path,
              "module PApp5206; import PLib5206;\n"
              "class P public static method Main(): int\n" + mainBody +
              "\nend Main; end P;\n");
    bezel::Driver driver;
    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bool ok = bezel::test::compileFileForTest(driver, aSrc.path, aExe.path,
                                              {dep}).success;
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    std::ifstream in(aErr.path);
    err->assign((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
    if (!ok || !std::ifstream(aExe.path).good()) return -1;
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(aExe.path.c_str(), aExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool has5206(const std::string& h, const char* n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

// The issue's repro: int is not convertible to string, no candidate applies.
BEZEL_TEST(xmod_imported_new_overload_5206,
           no_applicable_candidate_rejected_spec_22_6) {
    std::string err;
    int rc = run5206("var b := new B(6);\n return b.V;", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(has5206(err, "no overload of `new B`"));
    BEZEL_EXPECT_TRUE(!has5206(err, "#2658"));
}

// §22.5: an unknown named argument binds to no candidate.
BEZEL_TEST(xmod_imported_new_overload_5206,
           unknown_named_argument_rejected_spec_22_5) {
    std::string err;
    int rc = run5206("var b := new B(a := \"s\", zz := 2);\n return b.V;", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(has5206(err, "new B"));
    BEZEL_EXPECT_TRUE(!has5206(err, "#2658"));
}

// Controls (pass with and without the fix): valid overloaded calls still
// build and pick the right constructor.
BEZEL_TEST(xmod_imported_new_overload_5206,
           control_matching_overloads_accepted_and_run_spec_22_6) {
    std::string err;
    BEZEL_EXPECT_EQ(
        run5206("var b := new B(\"x\");\n var c := new B(\"x\", \"y\");\n"
                " return b.V * 10 + c.V;", &err),
        12);
}

BEZEL_TEST(xmod_imported_new_overload_5206,
           control_int_widens_to_long_candidate_spec_5_1) {
    std::string err;
    BEZEL_EXPECT_EQ(run5206("var w := new W(5);\n return w.V;", &err), 10);
}

BEZEL_TEST(xmod_imported_new_overload_5206,
           control_default_parameter_candidate_spec_22_5) {
    std::string err;
    BEZEL_EXPECT_EQ(
        run5206("var d := new D(1);\n var e := new D(1, 2);\n"
                " return d.V * 10 + e.V;", &err),
        63);
}

}  // namespace bezel::module
