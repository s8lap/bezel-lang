// Cross-module half of issue #2858's regression test.
//
// §17.6 (The increment and decrement operators) / §12.1 (Members are
// non-virtual by default) / §2.8.2 (Artifact interface) / §11.2 (Interface
// declaration), v0.240.
//
// The in-module cases live in tests/codegen/property_increment_virtual_dispatch_
// 2858_test.cpp; this file covers the shapes that need a real two-module build,
// because `lowerVirtualPropertyIncrDecr` reaches an IMPORTED property through a
// different route on both halves: the getter through `lowerVirtualPropertyGet`'s
// #503 fallback (`ps->decl` is null in the importer, so the return type comes
// off `ctx_.importedMemberType`) and the setter through `lowerVirtualProperty
// Set`'s #2406 twin of that fallback. Neither is exercised by a single-module
// program, and the blast radius of a change to the IncrementStmt arm across the
// artifact boundary is exactly the kind of thing that has been inferred wrongly
// before.
//
// Built the same way as this file's nearest sibling,
// tests/module/indexer_compound_iface_import_2836_test.cpp: `bezelc -c lib.bzl`
// then a real `--import` link and native run.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2858 {
    std::string path;
    explicit TempFile2858(std::string p) : path(std::move(p)) {}
    ~TempFile2858() { std::remove(path.c_str()); }
};

void writeSource2858(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2858(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun2858(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2858_" + std::to_string(std::rand());
    TempFile2858 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2858 aObj((dir / (stem + "_a.o")).string());
    writeSource2858(aSrc.path, libSrc);
    if (!compileToObj2858(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2858 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2858 bExe((dir / stem).string());
    writeSource2858(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — an IMPORTED `virtual`/`override` pair, incremented through a BASE-typed
// reference in the importing module. The importer has no `ast::PropertyDecl`
// for either accessor, so both halves ride the imported-member side tables.
// Pre-fix: 202 (the getter dispatched, the setter did not).
// Post-fix: 203.
// ===========================================================================
BEZEL_TEST(property_increment_import_2858, imported_virtual_property_increment) {
    const char* lib =
        "module PropLib2858T1; "
        "public class B2858T1 "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2858T1; "
        "public class D2858T1: B2858T1 "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D2858T1;";
    int r = compileImportAndRun2858(
        lib,
        "module App2858T1; import PropLib2858T1; "
        "public class C2858T1 "
        "  public static method Main(): int "
        "    using var d := new D2858T1(); "
        "    var b: B2858T1 := d; "
        "    b.P++; "
        "    return d._s; "
        "  end Main; "
        "end C2858T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 203);
}

// ===========================================================================
// T2 — an IMPORTED INTERFACE property, incremented through the interface. This
// is the itable half of the same fallback (#2406 reserves the setter slot for
// an imported interface property whose `ps->decl` is null). Pre-fix: 1 — not
// even the statically-bound answer. Post-fix: 203.
// ===========================================================================
BEZEL_TEST(property_increment_import_2858, imported_interface_property_increment) {
    const char* lib =
        "module PropLib2858T2; "
        "public interface IP2858T2 "
        "  property P: int; "
        "end IP2858T2; "
        "public class Impl2858T2: IP2858T2 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end Impl2858T2;";
    int r = compileImportAndRun2858(
        lib,
        "module App2858T2; import PropLib2858T2; "
        "public class C2858T2 "
        "  public static method Main(): int "
        "    using var d := new Impl2858T2(); "
        "    var q: IP2858T2 := d; "
        "    q.P++; "
        "    return d._s; "
        "  end Main; "
        "end C2858T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 203);
}

// ===========================================================================
// T3 — ACCEPT CONTROL: an imported NON-VIRTUAL property has no vtable slot, so
// the new branch declines and the existing cross-module lowering runs
// unchanged.
//
// ⚠ HONEST LABEL: "unchanged" here means unchanged and BROKEN. An imported
// non-virtual property's read-modify-write is SILENTLY DROPPED — `b.P++` and
// `b.P += 1` both store nothing (measured 0 where 102 is correct), while
// `b.P := 7` on the same property is correct (8). That is a PRE-EXISTING
// defect with its own root cause — `ps->decl` is null for an imported property
// and `computedSetterOf` requires an `ast::PropertyDecl`, so both
// read-modify-write paths fall through to the generic `lvalueAddr` fallback;
// the plain write works only because #2400 added
// `lowerImportedPropertySet`, and there is no compound/increment analogue of it
// (nor of #2509, which fixed exactly this for an imported INDEXER). MEASURED at
// 0 both BEFORE and AFTER this change, by a real two-module build. Filed as
// #2862; deliberately not fixed here — this case exists to pin that #2858 does
// not perturb it in either direction.
// ===========================================================================
BEZEL_TEST(property_increment_import_2858, imported_nonvirtual_unchanged) {
    const char* lib =
        "module PropLib2858T3; "
        "public class B2858T3 "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2858T3;";
    int r = compileImportAndRun2858(
        lib,
        "module App2858T3; import PropLib2858T3; "
        "public class C2858T3 "
        "  public static method Main(): int "
        "    using var b := new B2858T3(); "
        "    b.P++; "
        "    return b._s; "
        "  end Main; "
        "end C2858T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);   // #2862's dropped write — 102 once that is fixed
}

}  // namespace bezel::module
