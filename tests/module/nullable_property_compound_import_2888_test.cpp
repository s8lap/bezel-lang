// Spec-derived regression test for issue #2888 — the CROSS-MODULE row.
//
// §7.11 (Lifted operators) / §6.6 (Compound assignments) / §17.6 (The increment
// and decrement operators) / §2.8.2 (Artifact interface) / §23.3 (Block-form
// properties), v0.240.
//
// The issue listed `lowerImportedPropertyCompoundCore` (#2862's read-modify-
// write body for an IMPORTED non-virtual property) as the one caller it had NOT
// probed — inferred from code shape only. It is the same defect: that body calls
// `emitCompoundOp` with the unlifted `{i8 hasValue; T}` operands, so an imported
// `int?` property compound aborted the compile with
//
//     Call parameter type does not match function signature!
//     i32 3
//      { i8, i32 }  %N = call { { i8, i32 }, i1 }
//                        @llvm.sadd.with.overflow.sl_i8i32s({ i8, i32 } %M, i32 3)
//
// and the `++` spelling produced a fabricated `iN 1` from that body's own copy
// of §17.6's implicit `1`. Both are fixed by the same two one-definition moves
// the sibling codegen test documents (the §7.11 arm inside `emitCompoundOp`, and
// `emitImplicitOne`); this file pins them across a REAL two-module build
// (`Driver::compileObject` then `compileFile` with the object as a dependency,
// then a native run), because inference about the module boundary has been wrong
// repeatedly. Built the same way as its sibling
// tests/module/property_compound_import_2862_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2888 {
    std::string path;
    explicit TempFile2888(std::string p) : path(std::move(p)) {}
    ~TempFile2888() { std::remove(path.c_str()); }
};

void writeSource2888(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link or did not exit normally.
int compileImportAndRun2888(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2888_" + std::to_string(std::rand());
    TempFile2888 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2888 aObj((dir / (stem + "_a.o")).string());
    writeSource2888(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2888 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2888 bExe((dir / stem).string());
    writeSource2888(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — an IMPORTED non-virtual block-form `int?` property under §6.6's compound
// and §17.6's increment. Pre-fix BOTH aborted the compile (the app never built).
// The getter gives `_s + 100`, the setter stores `value + 1`, so `b.P += 3` is
// 0+100 +3 = 103, stored as 104; `b.P++` on a fresh object is 101 stored as 102.
// Post-fix: 7 — all three checks.
// ===========================================================================
BEZEL_TEST(nullable_property_compound_import_2888, imported_nullable_compound_7_11) {
    const char* lib =
        "module NPLib2888T1; "
        "public class B2888T1 "
        "  public _s: int? := 0; "
        "  public property P: int? "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2888T1;";
    int r = compileImportAndRun2888(
        lib,
        "module App2888T1; import NPLib2888T1; "
        "public class C2888T1 "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new B2888T1(); "
        "    b.P += 3; "
        "    if (b._s ?? -1) = 104 then acc := acc + 1; end; "
        "    using var c := new B2888T1(); "
        "    c.P++; "
        "    if (c._s ?? -1) = 102 then acc := acc + 2; end; "
        // §7.11: a null target propagates null through the imported accessors —
        // the setter never runs the underlying op, so `_s` stays null.
        "    using var d := new B2888T1(); "
        "    d._s := null; "
        "    d.P += 3; "
        "    if d._s = null then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end C2888T1;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T2 — ACCEPT CONTROL: the same shape with a NON-nullable `int` property must be
// bit-identical (#2862's own T1/T2 values: 102 for `++`, 102 for `+= 1`). Passes
// with and without the fix by construction; its job is to fail if the §7.11 arm
// over-reaches into a plain scalar property.
// ===========================================================================
BEZEL_TEST(nullable_property_compound_import_2888, imported_non_nullable_control) {
    const char* lib =
        "module NPLib2888T2; "
        "public class B2888T2 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2888T2;";
    int r = compileImportAndRun2888(
        lib,
        "module App2888T2; import NPLib2888T2; "
        "public class C2888T2 "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new B2888T2(); "
        "    b.P += 1; "
        "    if b._s = 102 then acc := acc + 1; end; "
        "    using var c := new B2888T2(); "
        "    c.P++; "
        "    if c._s = 102 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end C2888T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 3);
}

}  // namespace bezel::module
