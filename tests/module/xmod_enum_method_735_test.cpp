// Cross-module ENUM instance method dispatch (#735, §10 / §2.5 / §26-CG30).
//
// Bug: an imported enum's instance method (`Status.Ok.IsOk()`) returned a wrong
// value cross-module. The exporter lowers an enum instance method's `this` as
// the enum's underlying integer (i32) by value (CG30), but the importer
// declared the imported method's receiver as a pointer (ptrTy) — so the call
// passed a ptr where `bz$Status$IsOk(i32)` expected an i32, and the receiver was
// read as garbage. Fix: the importer declares an enum instance method's receiver
// as i32, matching the exporter (LlvmBackend_Setup.cpp + LlvmBackend_Calls.cpp).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile735 {
    std::string path;
    explicit TempFile735(std::string p) : path(std::move(p)) {}
    ~TempFile735() { std::remove(path.c_str()); }
};

void write735(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj735(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int importRun735(const std::string& bSrc, const std::string& bExe,
                 const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// #735: a cross-module enum instance method (bool- and int-returning) dispatches
// with the correct i32 receiver. IsOk()/Code() over imported Status.
BEZEL_TEST(xmod_enum_method_735, imported_enum_instance_method_dispatch) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz735_");
    TempFile735 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile735 aObj((dir / (stem + "_a.o")).string());

    write735(aSrc.path,
        "module EnumLib; "
        "public enum Status "
        "  Ok, Error, Pending; "
        "  public method IsOk(): bool => this = Status.Ok; "
        "  public method Code(): int => (if this = Status.Ok then 0 else 1); "
        "end Status;");

    BEZEL_EXPECT_TRUE(compileObj735(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile735 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile735 bExe((dir / (stem + "_b")).string());
    write735(bSrc.path,
        "module App; "
        "import EnumLib; "
        "class C public static method Main(): int "
        "  var s := Status.Ok; "
        "  var e := Status.Error; "
        "  var r := 0; "
        "  if s.IsOk() then r := r + 1; end; "
        "  if not e.IsOk() then r := r + 10; end; "
        "  r := r + e.Code() * 100; "
        "  return r; "   // 1 + 10 + 100 = 111
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun735(bSrc.path, bExe.path, aObj.path), 111);
}
