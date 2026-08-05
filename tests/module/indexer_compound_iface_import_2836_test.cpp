// Spec-derived regression test for issue #2836 — the CROSS-MODULE half.
//
// §23.4 (Indexers) / §11.2 (Interface declaration) / §6.6 (Compound
// assignments) / §2.8.2 (imported members), v0.240.
//
// The codegen half lives in tests/codegen/indexer_compound_virtual_dispatch_
// 2836_test.cpp; this file covers the one shape that needs a real two-module
// build to observe.
//
// THE DEFECT HERE. `Emitter::lowerIndexerCompound`'s IMPORTED-indexer branch
// (#2509) was gated `if (!pd) { if (kind != Class && kind != Record) return
// false; … }`. An IMPORTED INTERFACE's indexer also has `pd == null` (#2414:
// ModuleLoader reconstructs no ast::PropertyDecl), and its kind is `Interface`,
// so it was REJECTED by that gate — and, since the two remaining fallbacks
// (`lowerCollectionIndexerCompound`, `lvalueAddr`) do not handle a user indexer
// either, `q[i] OP= v` through an imported interface was SILENTLY DROPPED.
// MEASURED on 9983ddb via a real two-module build (`bezelc -c lib.bzl` then
// `bezelc --import lib.o app.bzl`): 0, where 203 is correct, no diagnostic.
//
// This is the exact shape #2422 fixed for `lowerIndexerGet` and
// `lowerIndexerSet` — "an IMPORTED INTERFACE-typed receiver also has pd == null
// … Let it fall through by excluding Interface from this gate" — applied to the
// third sibling, which never received it. The interface's accessors live in the
// itable, not in the `bz$<Class>$idx{get,set}$<arity>` direct-call ABI that gate
// guards, so excluding `Interface` routes it to the itable branch this fix adds.
//
// T2 and T3 are the ACCEPT CONTROLS for the widening: the imported CLASS shapes
// that already worked through that same gate must be untouched.
//
// Constants are all < 256 (a child process's exit status is WEXITSTATUS-masked
// to 8 bits) and chosen so a correct dispatch (203), a statically-bound answer
// (102) and a silently dropped write (0) are three distinct values.

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

struct TempFile2836 {
    std::string path;
    explicit TempFile2836(std::string p) : path(std::move(p)) {}
    ~TempFile2836() { std::remove(path.c_str()); }
};

void writeSource2836(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2836(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2509's own compileImportAndRun helper (this file's nearest sibling,
// tests/module/indexer_compound_increment_import_2509_test.cpp).
// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun2836(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2836_" + std::to_string(std::rand());
    TempFile2836 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2836 aObj((dir / (stem + "_a.o")).string());
    writeSource2836(aSrc.path, libSrc);
    if (!compileToObj2836(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2836 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2836 bExe((dir / stem).string());
    writeSource2836(bSrc.path, appSrc);
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
// T1 — the CORE cross-module repro: `q[i] += v` through an IMPORTED interface.
// Pre-fix: 0 (the write silently dropped by the Class||Record gate).
// Post-fix: 203 (get `_s + 200` = 200, +1 = 201, set `_s := 201 + 2`).
// ===========================================================================
BEZEL_TEST(indexer_compound_iface_import_2836, imported_interface_compound) {
    const char* lib =
        "module IdxIfaceLib2836T1; "
        "public interface IBox2836T1 "
        "  property [i: int]: int; "
        "end IBox2836T1; "
        "public class Impl2836T1: IBox2836T1 "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end Impl2836T1;";
    int r = compileImportAndRun2836(
        lib,
        "module App2836T1; import IdxIfaceLib2836T1; "
        "public class C2836T1 "
        "  public static method Main(): int "
        "    using var d := new Impl2836T1(); "
        "    var q: IBox2836T1 := d; "
        "    q[0] += 1; "
        "    return d._s; "
        "  end Main; "
        "end C2836T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 203);
}

// ===========================================================================
// T2 — ACCEPT CONTROL for the widened gate: an imported NON-VIRTUAL CLASS
// indexer still reaches #2509's direct-stub branch. 106 before and after
// (get `_s + 100` = 100, +5 = 105, set `_s := 105 + 1`).
// ===========================================================================
BEZEL_TEST(indexer_compound_iface_import_2836,
           imported_nonvirtual_class_unchanged) {
    const char* lib =
        "module IdxClsLib2836T2; "
        "public class B2836T2 "
        "  public _s: int := 0; "
        "  public property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2836T2;";
    int r = compileImportAndRun2836(
        lib,
        "module App2836T2; import IdxClsLib2836T2; "
        "public class C2836T2 "
        "  public static method Main(): int "
        "    using var b := new B2836T2(); "
        "    b[3] += 5; "
        "    return b._s; "
        "  end Main; "
        "end C2836T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 106);
}

// ===========================================================================
// T3 — ACCEPT CONTROL: an imported VIRTUAL class indexer through a BASE-typed
// reference. This one already dispatched pre-fix — what #2545 installs in
// `importedIndexerGetterFns_` for a virtual imported indexer is a vtable-
// DISPATCH wrapper, so #2509's stub branch was virtual all along. That is the
// disagreement the issue records ("the cross-module path is virtual and the
// LOCAL one is not; the two disagree"); this pins that the local fix closed the
// gap in the right direction and did not disturb the cross-module side. 203
// before and after.
// ===========================================================================
BEZEL_TEST(indexer_compound_iface_import_2836,
           imported_virtual_class_still_dispatches) {
    const char* lib =
        "module IdxVirtLib2836T3; "
        "public class B2836T3 "
        "  public _s: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B2836T3; "
        "public class D2836T3: B2836T3 "
        "  public override property [i: int]: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D2836T3;";
    int r = compileImportAndRun2836(
        lib,
        "module App2836T3; import IdxVirtLib2836T3; "
        "public class C2836T3 "
        "  public static method Main(): int "
        "    using var d := new D2836T3(); "
        "    var b: B2836T3 := d; "
        "    b[0] += 1; "
        "    return d._s; "
        "  end Main; "
        "end C2836T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 203);
}

}  // namespace bezel::module
