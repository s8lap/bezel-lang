// fix #6001 R1 / spec §12.2 (v0.338), QUOTED: "a derived class cannot add an
// overload to an inherited method's name, and an inherited overload set does
// not extend across the inheritance boundary". An override, in the importing
// module, of an IMPORTED generic base's member (`D: B<int>` overriding
// `B<T>.V(x: T)` as `V(x: int)`) replaces that member at the instantiation;
// once #6001 ranked candidates at their substituted signatures, the base
// member stayed beside it as a second, equally exact candidate and `d.V(1)`
// was rejected as ambiguous.
//
// Two-module (real Driver::compileObject + compileFile with a Dependency).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct Tmp6001x {
    std::string path;
    explicit Tmp6001x(std::string p) : path(std::move(p)) {}
    ~Tmp6001x() { std::remove(path.c_str()); }
};

void write6001x(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Result6001x {
    bool libBuilt = false;
    bool appCompiled = false;
    std::string err;
    std::string out;
    int code = -1;
};

Result6001x build6001x(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz6001x_");
    Tmp6001x libF((dir / (id + "_lib.bzl")).string());
    Tmp6001x libO((dir / (id + "_lib.o")).string());
    Tmp6001x appF((dir / (id + "_app.bzl")).string());
    Tmp6001x exeF((dir / id).string());
    write6001x(libF.path, libSrc);
    write6001x(appF.path, appSrc);
    Result6001x r;
    bezel::Driver driver;
    r.libBuilt = driver.compileObject(libF.path, libO.path).success &&
                 std::ifstream(libO.path).good();
    if (!r.libBuilt) return r;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libO.path;
    d.objectPath = libO.path;
    deps.push_back(d);

    std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    r.appCompiled =
        bezel::test::compileFileForTest(driver, appF.path, exeF.path, deps)
            .success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
    if (!r.appCompiled) return r;
    std::string cmd = exeF.path + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

const char* kLib =
    "module LibX; "
    "public class B<T> public virtual method V(x: T): int => 7; end B;";

}  // namespace

BEZEL_TEST(module_override_imported_generic_base_6001,
           override_of_imported_generic_base_member_runs_spec_12_2) {
    auto r = build6001x(
        kLib,
        "module AppY; import LibX; "
        "class D: B<int> public override method V(x: int): int => 8; end D; "
        "class P public static method Main(): int "
        "  using var d := new D(); Console.WriteLine(d.V(1)); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.err.find("ambiguous call"), std::string::npos);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("8\n"));
}

}  // namespace bezel::module
