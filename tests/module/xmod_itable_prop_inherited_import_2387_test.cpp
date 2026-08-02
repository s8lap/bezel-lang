// fix #2387 (§2.8.2/§2.8.3 rule 4 / §11.2 / §23, v0.235): `ensurePropertyAccessorFn`
// (LlvmBackend_Assign.cpp) gated on `dynamic_cast<const ast::PropertyDecl*>(ps->decl)`
// with no fallback, unconditionally returning nullptr for any IMPORTED
// PropertySymbol — ModuleLoader never reconstructs an ast::PropertyDecl for a
// body-less imported declaration (the getter/setter BODY is not reconstructed;
// only the resolved type rides the importedMemberType side table). This is
// reached from the itable-building loop (LlvmBackend_Setup.cpp declareVtables
// Pass D) via classPropertyByName, which correctly walks the FULL base-class
// chain (including an IMPORTED base) to resolve the property that satisfies an
// interface obligation. So a class that satisfies a LOCAL interface's property
// obligation via a property it INHERITS from an IMPORTED base class (rather
// than declaring the property itself) left its itable slot as
// ConstantPointerNull — calling through the interface reference (`b.Name`)
// then called address 0x0: SIGSEGV.
//
// This is the CODEGEN-side vertical partner of #2380 (which fixed the SEMA
// check, `checkAbstractImplementations`'s `propType` lambda, to correctly
// ACCEPT this exact program instead of silently accepting any type mismatch).
// #2380's T7 documented this crash without exercising it (compile-only) to
// keep the suite green; this file is the promised follow-up, filed as its own
// issue per #2380's file header and confirmed the NINTH instance of #2374's
// `astNode`/`decl`-only-gate defect-class audit.
//
// CONFIRMED SYMPTOM (verified by probe before writing this test, then
// mechanically by reverting the src/ fix — scratch-copied to a patch, NOT
// `git stash` — and re-running this exact filter):
//   Program received signal SIGSEGV, Segmentation fault.
//   0x0000000000000000 in ?? ()
//   #0  0x0000000000000000 in ?? ()
//   #1  0x0000000000400cce in main ()
// and, at the object level, the itable global (`itab$<Derived>$<Interface>`)
// had NO relocation at all pre-fix (a literal null constant); post-fix it
// carries an `R_X86_64_64` relocation to the exporter's
// `bz$<Lib>.<Base>$<Prop>$propget` symbol.
//
// ROOT-CAUSE NOTE ON THE FIX'S SECOND HALF (also confirmed by probe, not just
// code reading): the naive fallback — consulting the already-declared
// `importedPropGetterFns_` stub (LlvmBackend_Setup.cpp's
// declareImportedPropertyGetters, existing since #503 for the ORDINARY
// non-itable property-read path) — was not sufficient by itself.
// declareImportedPropertyGetters was called from LlvmBackend_impl.hpp AFTER
// declareVtables() RETURNED, but the itable-building Pass D that now consumes
// importedPropGetterFns_ runs INSIDE declareVtables(); the map was still empty
// when Pass D ran, and the fallback silently found nothing (still returned
// nullptr, an ordering bug that would have shipped a no-op "fix"). The actual
// fix moves the declareImportedPropertyGetters() call to INSIDE declareVtables(),
// after the buildVtable loops (local + imported) populate propGetSlot_ (which
// declareImportedPropertyGetters itself reads) and before Pass D.
//
// MATRIX (established by probe before writing this test; see the fix commit
// and issue #2387 for the full write-up):
//   - getter, LOCAL interface + imported-base-inherited property   -> WAS SIGSEGV, FIXED (T1/T2 below)
//   - getter, local-declared property (not inherited), control      -> always worked (T3)
//   - getter, same-module inherited property, control                -> always worked (T4)
//   - getter, two-levels-deep imported inheritance                   -> WAS SIGSEGV, FIXED (T2)
//   - setter, imported-base-inherited property                       -> STILL SIGSEGVs; no
//     `importedPropGetterFns_`-equivalent stub exists for setters at all
//     (declareImportedPropertyGetters only ever declared a GETTER external).
//     A correct fix needs a NEW, symmetric export/import mechanism
//     (emitExportedPropertySetters / declareImportedPropertySetters), out of
//     this fix's scope; filed separately (T5 below documents SEMA-accepts
//     without running the still-broken codegen path, mirroring #2380's T7).
//   - getter, IMPORTED interface + imported-base-inherited property  -> a
//     DIFFERENT, separately-filed bug: silently returns the WRONG VALUE (not
//     a crash) because ifacePropertyGetIndex_/ifacePropertySetIndex_/
//     ifaceEventIndex_ are populated only for LOCAL interfaces in
//     declareVtables' Pass 0 — the adjacent "mirror for imports" block only
//     mirrors ifaceMethodIndex_, not properties or events. Out of this fix's
//     scope (different code path); filed separately.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2387 {
    std::string path;
    explicit TempFile2387(std::string p) : path(std::move(p)) {}
    ~TempFile2387() { std::remove(path.c_str()); }
};

void writeSource2387(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2387(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object) -> exe, run it in a SEPARATE
// process (fork-isolated per tests/README.md's AOT harness discipline — a
// SIGSEGV in the run is contained to the child), return exit code (-1 on
// compile/link/run failure, -2 if the dependency itself could not be
// produced). Mirrors xmod_abstract_impl_prop_2380_test.cpp's
// compileImportAndRun2380.
int compileImportAndRun2387(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2387_" + std::to_string(std::rand());
    TempFile2387 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2387 aObj((dir / (stem + "_a.o")).string());
    writeSource2387(aSrc.path, libSrc);
    if (!compileToObj2387(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2387 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2387 bExe((dir / stem).string());
    writeSource2387(bSrc.path, appSrc);
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

// True iff B (importing A) compiles clean — no run.
bool compileImportSucceeds2387(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2387_ok_" + std::to_string(std::rand());
    TempFile2387 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2387 aObj((dir / (stem + "_a.o")).string());
    writeSource2387(aSrc.path, libSrc);
    if (!compileToObj2387(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2387 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2387 bExe((dir / (stem + "_b")).string());
    writeSource2387(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    return driver.compileFile(bSrc.path, bExe.path, deps).success;
}

}  // namespace

// ===========================================================================
// T1 — the exact #2387 repro: a GETTER, inherited from an IMPORTED base
// class, satisfying a LOCAL interface's property obligation. Pre-fix: SIGSEGV
// (null itable slot). Asserts the VALUE (`Name + 100`, not just `Name`, so a
// coincidental zero return cannot masquerade as success — the fix's own
// verification caught exactly this: the sibling imported-INTERFACE shape
// (a separate bug, not fixed here) silently returns 0 rather than crashing).
//
// Updated by #2542 (§11.2 / §23.3, v0.236): the interface obligation is now
// declared `readonly` to match the getter-only inherited implementation. The
// pre-#2542 read-write declaration was silently pinning the cross-module
// accessor-presence bug this fix closes (a getter-only member does not
// satisfy a read-write interface property obligation, even when the witness
// is inherited from an imported base class).
// ===========================================================================
BEZEL_TEST(xmod_itable_prop_inherited_import_2387,
           getter_inherited_from_imported_base_local_iface_runs_and_dispatches) {
    int r = compileImportAndRun2387(
        "module BasePropLib2387T1; "
        "public class BaseWithProp2387T1 "
        "  public property Name: int => 1; "
        "end BaseWithProp2387T1;",
        "module App2387T1; import BasePropLib2387T1; "
        "public interface IHasName2387T1 "
        "  readonly property Name: int; "
        "end IHasName2387T1; "
        "public class Derived2387T1: BaseWithProp2387T1, IHasName2387T1 "
        "end Derived2387T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2387T1 := new Derived2387T1(); "
        "    return b.Name + 100; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 101);
}

// ===========================================================================
// T2 — TWO LEVELS of imported inheritance: the interface-satisfying property
// is declared on an imported base, inherited by a SECOND imported class
// (itself with no interface), which the LOCAL class then extends. Guards
// against a fix that only walks one level (e.g. checking the immediate base's
// decl rather than using classPropertyByName's full-chain walk).
//
// Updated by #2542 (§11.2 / §23.3, v0.236): the interface obligation is now
// declared `readonly` to match the getter-only inherited implementation (see
// T1's note).
// ===========================================================================
BEZEL_TEST(xmod_itable_prop_inherited_import_2387,
           getter_inherited_two_levels_through_imported_bases_runs_and_dispatches) {
    int r = compileImportAndRun2387(
        "module BasePropLib2387T2; "
        "public class BaseWithProp2387T2 "
        "  public property Name: int => 1; "
        "end BaseWithProp2387T2; "
        "public class MiddleDerived2387T2: BaseWithProp2387T2 "
        "end MiddleDerived2387T2;",
        "module App2387T2; import BasePropLib2387T2; "
        "public interface IHasName2387T2 "
        "  readonly property Name: int; "
        "end IHasName2387T2; "
        "public class Derived2387T2: MiddleDerived2387T2, IHasName2387T2 "
        "end Derived2387T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2387T2 := new Derived2387T2(); "
        "    return b.Name + 200; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 201);
}

// ===========================================================================
// T3 — CONTROL: a property declared DIRECTLY on the (local) implementing
// class, not inherited, satisfying an interface imported from another
// module. Guards against the fix breaking the ordinary, already-working path
// (`ensurePropertyAccessorFn` must still take its normal `pd`-populated
// branch when the property genuinely IS local).
//
// Updated by #2542 (§11.2 / §23.3, v0.236): the interface obligation is now
// declared `readonly` to match the getter-only local implementation (see T1's
// note).
// ===========================================================================
BEZEL_TEST(xmod_itable_prop_inherited_import_2387,
           getter_declared_locally_control_runs_and_dispatches) {
    int r = compileImportAndRun2387(
        "module PropIfaceLib2387T3; "
        "public interface IHasName2387T3 "
        "  readonly property Name: int; "
        "end IHasName2387T3;",
        "module App2387T3; import PropIfaceLib2387T3; "
        "public class GoodProp2387T3: IHasName2387T3 "
        "  private _n: int := 77; "
        "  public property Name: int => _n; "
        "end GoodProp2387T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2387T3 := new GoodProp2387T3(); "
        "    return b.Name - 77; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T4 — CONTROL: a property inherited from a SAME-MODULE (local) base class,
// satisfying a local interface. This is the shape the issue's own repro used
// to establish the crash was cross-module-specific ("The SAME-MODULE
// equivalent... compiles AND runs correctly"). No import involved at all;
// guards against a fix that accidentally regresses ordinary local dispatch.
// ===========================================================================
BEZEL_TEST(xmod_itable_prop_inherited_import_2387,
           getter_inherited_same_module_control_runs_and_dispatches) {
    // This shape needs no import at all (single-module program), so drive the
    // Driver directly rather than compileImportAndRun2387/compileImportSucceeds2387
    // (both of which require a real dependency object).
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2387_t4_" + std::to_string(std::rand());
    TempFile2387 src((dir / (stem + ".bzl")).string());
    TempFile2387 exe((dir / stem).string());
    writeSource2387(src.path,
                    "module SameMod2387T4; "
                    "public class BaseWithPropSM2387T4 "
                    "  public property Name: int => 1; "
                    "end BaseWithPropSM2387T4; "
                    // §11.2/§23.3 (#2413): BaseWithPropSM2387T4's `Name` is
                    // getter-only and this test never writes through the
                    // interface, so IHasNameSM2387T4's obligation is
                    // declared `readonly` — a getter-only implementation of
                    // a read-write interface property is now a compile
                    // error (#2413's own fix, the LOCAL/single-module
                    // sibling of this file's cross-module #2387 defect).
                    "public interface IHasNameSM2387T4 "
                    "  readonly property Name: int; "
                    "end IHasNameSM2387T4; "
                    "public class DerivedSM2387T4: BaseWithPropSM2387T4, "
                    "IHasNameSM2387T4 end DerivedSM2387T4; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b: IHasNameSM2387T4 := new DerivedSM2387T4(); "
                    "    return b.Name - 1; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL (compiles only — see the file header's matrix for why
// the value is not asserted): a SETTABLE property inherited from an imported
// base class, satisfying a local interface's setter obligation, still
// compiles clean (the #2380 sema fix accepts it — unaffected by, and not
// exercised by, this codegen fix). Does NOT run the program: this shape hits
// a SEPARATE, STILL-PRESENT codegen null-itable-slot crash on the SETTER slot
// specifically (confirmed by probe: a write-only repro segfaults identically
// to the pre-fix getter crash) — no `importedPropGetterFns_`-equivalent stub
// exists for setters, so this fix's fallback cannot reach it. Filed as its
// own issue (mirrors #2380's T7 pattern exactly).
// ===========================================================================
BEZEL_TEST(xmod_itable_prop_inherited_import_2387,
           setter_inherited_from_imported_base_correct_still_compiles) {
    bool ok = compileImportSucceeds2387(
        "module BasePropLib2387T5; "
        "public class BaseWithPropSet2387T5 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end BaseWithPropSet2387T5;",
        "module App2387T5; import BasePropLib2387T5; "
        "public interface IHasNameSet2387T5 "
        "  property Name: int; "
        "end IHasNameSet2387T5; "
        "public class Derived2387T5: BaseWithPropSet2387T5, IHasNameSet2387T5 "
        "end Derived2387T5; "
        "public class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(ok);
}

}  // namespace bezel::module
