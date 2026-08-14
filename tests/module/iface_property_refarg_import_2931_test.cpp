// Cross-module half of issue #2931's regression test.
//
// §11.2 (Interface declaration) / §23.1 (Auto-properties) / §22.3 (Parameter
// modifiers) / §2.8.2 (Artifact interface) / §2.8.3 rule 4, v0.240.
//
// The in-module cases live in
// tests/codegen/interface_property_lvalue_addr_2931_test.cpp. This file exists
// because the misclassification #2931 fixes reaches an IMPORTED interface
// property by a DIFFERENT route: the local answer comes from
// `Emitter::isAutoProperty`, which reads `ast::PropertyDecl::bodyKind` and is
// therefore always false for an imported symbol (no decl crosses the artifact
// boundary), while the imported answer rides the `MetaMember::isAutoProperty`
// WIRE BIT — set by `ModuleMetadataBuilder.cpp` as `out.isAutoProperty =
// pd->bodyKind == ast::PropertyBodyKind::Auto` for EVERY property including an
// interface's, and read back through `isAutoPropertyStorage`'s decl-null
// fallback. A fix placed on the local predicate alone would leave this half
// broken, which is exactly why the exclusion lives in `storageMemberOf`, where
// both sources of the classification meet.
//
// Built the same way as this file's nearest sibling,
// tests/module/property_increment_import_2858_test.cpp: `bezelc -c lib.bzl`
// then a real `--import` link and native run.

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

struct TempFile2931 {
    std::string path;
    explicit TempFile2931(std::string p) : path(std::move(p)) {}
    ~TempFile2931() { std::remove(path.c_str()); }
};

void writeSource2931(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2931(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun2931(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2931_" + std::to_string(std::rand());
    TempFile2931 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2931 aObj((dir / (stem + "_a.o")).string());
    writeSource2931(aSrc.path, libSrc);
    if (!compileToObj2931(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2931 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2931 bExe((dir / stem).string());
    writeSource2931(bSrc.path, appSrc);
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
// T1 — an IMPORTED interface and an IMPORTED implementing class. `IP` is backed
// by `_v` (field index 1) and `_z` is index 0, so the pre-fix ordinal write
// into index 0 is visible as BOTH the target being unwritten and its neighbour
// being clobbered.
// Post-fix: 3 (`_v = 5`, `_z = 0`).
// ===========================================================================
BEZEL_TEST(iface_property_refarg_import_2931, imported_iface_property_ref_arg_22_3) {
    const char* lib =
        "module IfcLib2931T1; "
        "public interface IHasQ2931T1 "
        "  property IP: int; "
        "end IHasQ2931T1; "
        "public class Box2931T1: IHasQ2931T1 "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box2931T1;";
    int r = compileImportAndRun2931(
        lib,
        "module App2931T1; import IfcLib2931T1; "
        "public class C2931T1 "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box2931T1(); "
        "    var i: IHasQ2931T1 := b; "
        "    C2931T1.Bump(ref i.IP); "
        "    if b._v = 5 then acc := acc + 1; end; "
        "    if b._z = 0 then acc := acc + 2; end; "
        "    return acc; "
        "  end Main; "
        "end C2931T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 3);
}

// ===========================================================================
// T2 — an IMPORTED interface implemented by a LOCAL class. The interface
// PropertySymbol is the imported (decl-null) one — so it is the wire bit that
// classifies it — while the storage being corrupted belongs to a class the
// importing module lays out itself. Two unrelated fields come first so the
// pre-fix ordinal-0 write cannot coincide with the property's own backing.
// Post-fix: 1.
// ===========================================================================
BEZEL_TEST(iface_property_refarg_import_2931, imported_iface_local_impl_ref_arg_11_2) {
    const char* lib =
        "module IfcLib2931T2; "
        "public interface IHasQ2931T2 "
        "  property IP: int; "
        "end IHasQ2931T2;";
    int r = compileImportAndRun2931(
        lib,
        "module App2931T2; import IfcLib2931T2; "
        "public class Local2931T2: IHasQ2931T2 "
        "  public _f0: int := 0; "
        "  public _f1: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Local2931T2; "
        "public class C2931T2 "
        "  public static method Bump(ref n: int): int "
        "    n := n + 5; "
        "    return n; "
        "  end Bump; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Local2931T2(); "
        "    var i: IHasQ2931T2 := b; "
        "    C2931T2.Bump(ref i.IP); "
        "    if b._v = 5 and b._f0 = 0 and b._f1 = 0 then acc := acc + 1; end; "
        "    return acc; "
        "  end Main; "
        "end C2931T2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 1);
}

}  // namespace bezel::module
