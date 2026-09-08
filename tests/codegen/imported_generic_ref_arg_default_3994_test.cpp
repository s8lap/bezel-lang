// Quick verification for issue #3994 — imported generic record with ref-typed type arg
// computes the WRONG value silently for default(T)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const std::filesystem::path& wtDir3994() {
    return wtScratchDir("wt-3994-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct LibHandle {
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& libSrc) {
    LibHandle h;
    auto dir = wtDir3994();
    std::string stem = unique_stem("bezel_3994lib_");
    std::string srcPath = (dir / (stem + "_lib.bzl")).string();
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(srcPath, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(srcPath, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    bool ok = false;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& libObjPath, const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    auto dir = wtDir3994();
    std::string stem = unique_stem("bezel_3994app_");
    std::string srcPath = (dir / (stem + "_app.bzl")).string();
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(srcPath, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(srcPath, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

int runExe(const std::string& exePath) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exePath.c_str(), exePath.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}
}

BEZEL_TEST(imported_generic_ref_arg_default_3994,
           imported_default_returns_9) {
    const std::string lib =
        "module LibGNull3994; "
        "public class WidgetGN3994 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetGN3994; "
        "public record BoxGN3994<T> "
        "  public readonly property V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "  public method IsVNull(): bool => V = null; "
        "end BoxGN3994; ";
    const std::string app =
        "module AppGNull3994; "
        "import LibGNull3994; "
        "class Program "
        "  public static method Main(): int "
        "    var x := default(LibGNull3994.BoxGN3994<LibGNull3994.WidgetGN3994?>); "
        "    if x.IsVNull() then return 9; end if; "
        "    return 1; "
        "  end Main; "
        "end Program; ";
    LibHandle libH = buildLib(lib);
    if (!libH.ok) {
        fprintf(stderr, "LIB COMPILE FAILED\n");
    }
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    if (!a.ok) {
        fprintf(stderr, "APP COMPILE FAILED\n");
    }
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    BEZEL_EXPECT_EQ(runExe(a.exePath), 9);
}
