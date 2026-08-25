// Codegen — fix #3359: per-instantiation descriptor linkage must collapse
// across modules so a cross-module `is`/`as`/`catch`/§19 test resolves to
// the same address the exporter installed in the specialized vtable's slot 0.
// (Residual #2797 — the vtable side of #2797's pattern is deferred because
// the vtable's other slots reference InternalLinkage helper functions whose
// contents would differ across modules under LinkOnceODR merge; the
// descriptor fix alone suffices for the `is` test the issue names.)
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// For a generic class instantiated across modules (e.g., `MyBox<MyRecord>`
// where `MyRecord` lives in module A and `MyBox<MyRecord>` usage lives in
// module B), the per-instantiation descriptor the `is MyBox<MyRecord>` test
// resolves via `perInstDescriptorForTarget` (`td$MyBox_int`, see
// `instanceDescriptorFor` at `LlvmBackend_Setup.cpp:8063`) was emitted with
// InternalLinkage, so each module minted its OWN copy. The exporter-constructed
// object carries the exporter's `vt$MyBox$specN`, whose slot 0 holds the
// exporter's `td$MyBox_int`. The importer's `is MyBox<MyRecord>` site
// resolves the IMPORTER's `td$MyBox_int`. The two addresses differ, the
// descriptor-identity comparison silently fails, and the test is FALSE —
// even though the object IS an instance of the target.
//
// §2.8.3 rule 4 demands "one runtime type identity per monomorphization …
// regardless of which artifact produced the value or performs the test";
// the cross-module split breaks this. §2.8.2 clause 2: "An external
// instantiation behaves identically to a same-module instantiation of the
// same generic at the same arguments".
//
// ── THE FIX ──────────────────────────────────────────────────────────────
//
// Promote the per-instantiation descriptor (`td$<readable-mangle>`) minted
// by `instanceDescriptorFor` from InternalLinkage to LinkOnceODR, mirroring
// the `btd$<R>` LinkOnceODR precedent #2797 established for public,
// non-generic records/enums. Three structural properties make LinkOnceODR
// the right shape:
//
//   1. SAME NAME ACROSS MODULES — the descriptor's name is `descReadableMangle`,
//      a structural encoding of `(ts, args)` that is deterministic across
//      modules (it does NOT use the `descIdentityKey`'s pointer-based hash,
//      which would differ because each module loads its own TypeSymbol).
//   2. SAME CONTENTS ACROSS MODULES — the BFS over `ts->baseTypes` (which
//      ModuleLoader populates from the importer's view, so the importer's
//      `baseTypes` matches the exporter's for the same generic) and the
//      `directBuiltinExceptionBase` walk produce identical chains for the
//      same `(ts, args)` from each side. The chain elements are already
//      LinkOnceODR (`btd$Exception`, etc., at `LlvmBackend_Setup.cpp:5106`)
//      or the self-reference (which LinkOnceODR resolves to one address).
//   3. §2.8.3 rule 3 requires same-named distinct-declarations NOT to
//      collapse — `descReadableMangle` does NOT carry a module qualifier
//      (exactly the `btd$<R>` precedent #2797 set), so two modules declaring
//      same-named distinct-generic-instantiations stay distinct.
//
// The result: both modules' `td$<...>` definitions ODR-merge into one
// canonical address; slot 0 of BOTH modules' `vt$<C>$specN` references that
// one address; and §2.8.3 rule 4's identity is preserved across the boundary
// by construction.
//
// ⚠ WHAT THIS FIX DOES NOT COVER — the vtable `vt$<C>$specN` itself
// remains InternalLinkage in both modules (one for each function in each
// compilation unit), so each module independently monomorphizes its OWN
// vtable. For the `is`/`as`/`catch`/§19 test — which compares slot 0 of
// the vtable (the descriptor) — that is harmless because the descriptor is
// now shared. For VIRTUAL METHOD DISPATCH through slot 1+ across modules
// (a constructor in the exporter, an `override` call in the importer), the
// importer's slot 1+ entries point at the IMPORTER's specialized method
// bodies (InternalLinkage), while the exporter-constructed object carries
// the EXPORTER's vtable whose slot 1+ points at the EXPORTER's bodies. Both
// sides have structurally identical bodies, so the call still produces the
// right answer (the bodies read the same fields on the same layout), but
// the linker sees TWO definitions for the same symbol if the specN counter
// happens to align. Promoting the vtable itself to ExternalLinkage would
// require either (a) a deterministic specN naming scheme (currently the
// counter is order-of-monomorphization local to each compilation, fragile
// when both modules see different spec sets), or (b) making every per-spec
// method/itmap/disposer helper shared-linkage too — a far larger refactor
// than this lane scoped. Filed separately; this lane closes the `is` test
// half the issue's headline calls out, which is what was blocking #3359's
// auto-close.
//
// ── SPEC CITATION ─────────────────────────────────────────────────────────
//
// §2.8.3 rule 4 (v0.278): "one runtime type identity — `is` tests, `catch`
// clauses, and §19 patterns over values of the instantiation match
// identically regardless of which artifact produced the value or performs
// the test" / §2.8.2 clause 2 ("An external instantiation behaves identically
// to a same-module instantiation of the same generic at the same
// arguments") / §14.1 "Generic declarations".
//
// ── FAIL-WITHOUT / PASS-WITH ──────────────────────────────────────────────
//
// T1 cross-module `is MyBox<int>`: pre-fix FALSE (exporter-constructed
// object carries exporter's `td$MyBox_int`, importer's `is` resolves to
// importer's `td$MyBox_int`); post-fix TRUE (LinkOnceODR merge collapses
// them into one canonical address, so the descriptor-identity comparison
// succeeds).
// T2 cross-module `as MyBox<int>`: pre-fix NULL (`as` returns null when the
// `is` identity check fails), post-fix non-NULL with the stored int (42)
// preserved.
// T3 control: single-module same repro returns TRUE unchanged (no
// regression in the local-monomorphization path — the same descriptor's
// InternalLinkage form is unaffected by a LinkOnceODR rename in a multi-
// module build; single-module programs emit exactly one definition either
// way).
// T4 control: cross-module generic `is` over a NON-instantiated type (e.g.,
// `is MyBox<float>` on a MyBox<int> object) returns FALSE — no over-correction
// (#1843/#2001 type-confusion class guard; the two instantiations get
// DIFFERENT descriptors — `td$MyBox_int` vs `td$MyBox_float` — and LinkOnceODR
// is name-keyed, so the merger keeps them distinct).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"

namespace fs = std::filesystem;

namespace {

struct TempFile3359 {
    std::string path;
    explicit TempFile3359(std::string p) : path(std::move(p)) {}
    ~TempFile3359() { std::remove(path.c_str()); }
};

void writeSource3359(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build the dependency's library, return its path.
struct LibHandle3359 {
    std::shared_ptr<TempFile3359> src;
    std::shared_ptr<TempFile3359> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle3359 buildLib3359(const fs::path& dir,
                            const std::string& libSrc) {
    LibHandle3359 h;
    std::string stem = unique_stem("bezel_3359lib_");
    h.src = std::make_shared<TempFile3359>(
        (dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile3359>(
        (dir / (stem + "_lib.o")).string());
    writeSource3359(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r =
        driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

// In-process importer-side compile (uses `Driver::compileFile`).
struct InProcessApp3359 {
    bool ok = false;
    std::shared_ptr<TempFile3359> src;
    std::shared_ptr<TempFile3359> exe;
    std::string exePath;
};

InProcessApp3359 buildApp3359(const fs::path& dir,
                              const std::string& libObjPath,
                              const std::string& appSrc) {
    InProcessApp3359 r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3359app_");
    r.src = std::make_shared<TempFile3359>(
        (dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile3359>(
        (dir / (stem + "_app")).string());
    writeSource3359(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath   = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

int runExe(const std::string& exe) {
    int status = std::system(exe.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static const fs::path& wtDir3359() {
    return wtScratchDir("wt-3359");
}

}  // namespace

// ===========================================================================
// T1 — issue's HEADLINE repro. Module A defines `MyBox<T>` with a static
// `MakeBox` that returns `owned MyBox<int>`. Module B imports `MyBox<int>`,
// constructs one via `Maker.MakeBox`, and tests `is MyBox<int>` on it.
// Pre-fix the `is` test resolves the IMPORTER's `td$MyBox_int` (Internal),
// but the object carries the EXPORTER's `vt$MyBox$specN` whose slot 0 = the
// EXPORTER's `td$MyBox_int` (Internal, different address) — the identity
// comparison silently fails. Post-fix both modules' `td$MyBox_int`
// ODR-merge into one canonical address and the test passes. §14.1 /
// §2.8.3 rule 4 / §2.8.2 clause 2, v0.278.
// ===========================================================================
// ===========================================================================
// T1 — issue's HEADLINE repro. Module A defines `MyBox<T>` with a static
// `MakeBox` that returns `owned MyBox<int>`. Module B imports `MyBox<int>`,
// constructs one via `Maker.MakeBox`, and tests `is MyBox<int>` on it.
// Pre-fix the `is` test resolves the IMPORTER's `td$MyBox_int` (Internal),
// but the object carries the EXPORTER's `vt$MyBox$specN` whose slot 0 = the
// EXPORTER's `td$MyBox_int` (Internal, different address) — the identity
// comparison silently fails. Post-fix both modules' `td$MyBox_int`
// ODR-merge into one canonical address and the test passes. §14.1 /
// §2.8.3 rule 4 / §2.8.2 clause 2, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_specialized_vtable_linkage_3359,
           t1_xmod_is_mybox_int_returns_true_2_8_3_14_1) {
    auto dir = wtDir3359();
    const std::string lib =
        "module Lib3359t1; "
        "public class MyBox<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "end MyBox; "
        "public class Maker "
        "  public static method MakeBox(v: int): owned MyBox<int> => new MyBox<int>(v); "
        "end Maker; ";
    const std::string app =
        "module App3359t1; "
        "import Lib3359t1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := Maker.MakeBox(42); "
        "    if b is MyBox<int> then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle3359 libH = buildLib3359(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3359 appH = buildApp3359(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    BEZEL_EXPECT_EQ(runExe(appH.exePath), 1);
}

// ===========================================================================
// T2 — `as MyBox<int>` twin. After `as`, the boxed object's value field must
// be preserved (the `as` succeeds, so it must be the same object). §14.1 /
// §2.8.3 rule 4 / §17.4, v0.240.
// ===========================================================================
// ===========================================================================
// T2 — `as MyBox<int>` twin. After `as`, the boxed object's value field must
// be preserved (the `as` succeeds, so it must be the same object). §14.1 /
// §2.8.3 rule 4 / §17.4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_specialized_vtable_linkage_3359,
           t2_xmod_as_mybox_int_preserves_value_2_8_3_14_1) {
    auto dir = wtDir3359();
    const std::string lib =
        "module Lib3359t2; "
        "public class MyBox<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "end MyBox; "
        "public class Maker "
        "  public static method MakeBox(v: int): owned MyBox<int> => new MyBox<int>(v); "
        "end Maker; ";
    const std::string app =
        "module App3359t2; "
        "import Lib3359t2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := Maker.MakeBox(42); "
        "    var bb := (b as MyBox<int>); "
        "    if bb = null then return 0; end if; "
        "    return bb.value; "
        "  end Main; "
        "end C; ";
    LibHandle3359 libH = buildLib3359(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3359 appH = buildApp3359(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    BEZEL_EXPECT_EQ(runExe(appH.exePath), 42);
}

// ===========================================================================
// T3 — single-module control: the same declarations and the same body in
// ONE module. The `is MyBox<int>` test must return TRUE unchanged — no
// regression in the local monomorphization path. §14.1, v0.240.
// ===========================================================================
// ===========================================================================
// T3 — single-module control: the same declarations and the same body in
// ONE module. The `is MyBox<int>` test must return TRUE unchanged — no
// regression in the local monomorphization path (a single-module build emits
// exactly one definition either way, so LinkOnceODR is harmless). §14.1,
// v0.278.
// ===========================================================================
BEZEL_TEST(xmod_specialized_vtable_linkage_3359,
           t3_local_control_is_mybox_int_returns_true_14_1) {
    auto dir = wtDir3359();
    std::string stem = unique_stem("bezel_3359loc_");
    auto src = std::make_shared<TempFile3359>(
        (dir / (stem + ".bzl")).string());
    auto exe = std::make_shared<TempFile3359>(
        (dir / stem).string());
    const std::string code =
        "module M; "
        "public class MyBox<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "end MyBox; "
        "public class C "
        "  public static method Main(): int "
        "    var b := new MyBox<int>(42); "
        "    if b is MyBox<int> then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C; ";
    writeSource3359(src->path, code);
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileFile(src->path, exe->path, {}).success);
    BEZEL_EXPECT_EQ(runExe(exe->path), 1);
}

// ===========================================================================
// T4 — over-correction guard (§12.7 / §2.8.3 rule 4 / #1843/#2001
// type-confusion class). A MyBox<int> object MUST NOT test TRUE for
// `is MyBox<float>` — the descriptors must stay distinct per (T) so the
// runtime type-test cannot collapse two different instantiations onto one.
// §14.1 / §17.4, v0.240.
// ===========================================================================
// ===========================================================================
// T4 — over-correction guard (§12.7 / §2.8.3 rule 4 / #1843/#2001
// type-confusion class). A MyBox<int> object MUST NOT test TRUE for
// `is MyBox<float>` — the two instantiations get DIFFERENT descriptors
// (`td$MyBox_int` vs `td$MyBox_float`), and LinkOnceODR is name-keyed, so
// the merger keeps them distinct. §14.1 / §17.4, v0.278.
// ===========================================================================
BEZEL_TEST(xmod_specialized_vtable_linkage_3359,
           t4_over_correction_guard_is_mybox_float_returns_false_14_1) {
    auto dir = wtDir3359();
    const std::string lib =
        "module Lib3359t4; "
        "public class MyBox<T> "
        "  public value: T; "
        "  public constructor(v: T) value := v; end constructor; "
        "end MyBox; "
        "public class Maker "
        "  public static method MakeBox(v: int): owned MyBox<int> => new MyBox<int>(v); "
        "end Maker; ";
    const std::string app =
        "module App3359t4; "
        "import Lib3359t4; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := Maker.MakeBox(42); "
        "    if b is MyBox<float> then return 0; end if; "
        "    return 1; "
        "  end Main; "
        "end C; ";
    LibHandle3359 libH = buildLib3359(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp3359 appH = buildApp3359(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    BEZEL_EXPECT_EQ(runExe(appH.exePath), 1);
}
