// fix #2400 (§2.8.2/§2.8.3 rule 4 / §11.2 / §23, v0.235): the SETTER twin of
// #2387's itable getter fix. `ensurePropertyAccessorFn` (LlvmBackend_Assign.cpp)
// gated on `dynamic_cast<const ast::PropertyDecl*>(ps->decl)` with a fallback
// added for `isSet=false` only (#2387) — the `isSet=true` branch still
// unconditionally returned nullptr for any IMPORTED PropertySymbol, because
// (unlike the getter) there was no setter-side equivalent of
// `importedPropGetterFns_`/`declareImportedPropertyGetters` to fall back to at
// all: `emitExportedPropertyGetters` (LlvmBackend_Setup.cpp) only ever exported
// a GETTER (confirmed empirically: `nm` on a compiled library object with a
// settable property showed `...$propget` but no `...$propset` symbol
// whatsoever). Cross-module SETTING of a non-virtual imported property had no
// ABI surface at all — independent of interfaces/itables.
//
// Two distinct symptoms of that single missing mechanism, both confirmed by
// probe before writing this test:
//   1. ITABLE shape (this file's T1/T2/T3/T4): a class satisfies a LOCAL
//      interface's SETTABLE property obligation via a property it INHERITS
//      from an IMPORTED base class. The itable's setter slot is installed as
//      ConstantPointerNull; `b.Name := v` through the interface reference
//      SIGSEGVs identically to #2387's pre-fix getter crash:
//        Program received signal SIGSEGV, Segmentation fault.
//        0x0000000000000000 in ?? ()
//        #0  0x0000000000000000 in ?? ()
//        #1  0x0000000000400cce in main ()
//      At the object level (verified via `nm`/`readelf -x` on a compiled
//      probe): the exporter's object carried `bz$<Lib>.<Base>$Name$propget`
//      but no `...$propset` symbol at all; the itable global
//      (`itab$<Derived>$<Iface>`, 16 bytes = 2 slots) had its GETTER slot
//      (bytes 0-7) correctly relocated to the exporter's `$propget` symbol
//      (post-#2387) but its SETTER slot (bytes 8-15) as a literal null
//      constant — no relocation at all.
//   2. DIRECT (non-itable) shape (T5): `importedObj.Prop := v` where Prop is
//      accessed through the imported class's OWN type, not an interface. This
//      does not crash — it silently DROPS the write. computedSetterOf
//      (LlvmBackend_Assign.cpp) requires an ast::PropertyDecl and returns null
//      for an imported property, so lowerPropertySet bails; before this fix
//      nothing else recognized the shape either, so the assignment statement
//      fell through to the generic lvalueAddr fallback, which cannot resolve a
//      PropertySymbol target (it is not a field) and silently no-ops. Verified
//      by probe: `d.Name := 55; return d.Name - 55;` (backing field starts at
//      1) returned -54 (WEXITSTATUS-masked to 202) — the READ correctly
//      returned the untouched original value via the pre-existing
//      importedPropGetterFns_ getter path (issue #503), proving the WRITE
//      never happened at all.
//
// This fix adds (mirroring the getter side exactly):
//   - `emitExportedPropertySetters()` (LlvmBackend_Setup.cpp): exports
//     `bz$<Class>$<Prop>$propset(ptr this, T value) -> void` for every public
//     non-virtual settable property on a local class.
//   - `declareImportedPropertySetters()` (LlvmBackend_Setup.cpp): declares the
//     external stub and populates a new `importedPropSetterFns_` map. Called
//     from the SAME call site #2387 moved declareImportedPropertyGetters to —
//     INSIDE declareVtables(), after propSetSlot_ is populated, before Pass D
//     (the itable-building loop, the map's first consumer) — #2387 already
//     discovered that a call site placed after declareVtables() returns
//     leaves the map empty during Pass D and silently defeats the fallback;
//     confirmed the same ordering requirement applies here by testing the
//     naive (mis-ordered) version first and observing no change.
//   - `ensurePropertyAccessorFn`'s `isSet==true` branch of the `!pd` fallback
//     now consults `importedPropSetterFns_` (LlvmBackend_Assign.cpp), fixing
//     the ITABLE shape.
//   - A new `lowerImportedPropertySet`/`lowerImportedPropertySetValue`
//     (LlvmBackend_Assign.cpp), wired into the `:=` assignment path
//     (LlvmBackend_Stmts.cpp) right after `lowerPropertySet` fails, fixing the
//     DIRECT shape.
//
// `propertyHasSetter` (LlvmBackend_impl.hpp) was audited as instructed by this
// issue's text ("propertyHasSetter's own ps->decl-gate as a further instance")
// — it is unconditionally false for ANY imported PropertySymbol, but every
// call site relevant to THIS fix's shapes (Pass 0's local-interface loop,
// Pass D's interface-own-property check) operates on a LOCAL interface's own
// PropertyDecl (present) in every test below, so it is unaffected here. Its
// gate DOES break the separate case of an IMPORTED interface's own settable
// property (no itable setter slot reserved at all, regardless of where the
// implementation lives) — already confirmed and filed as #2406, deliberately
// NOT touched by this fix or its tests (a different code path: Pass 0's
// import-mirror block, #2401/#2401-adjacent territory) to keep this PR's scope
// to the export/import ABI gap #2400 itself describes.
//
// Mechanically verified fail-without/pass-with: reverted the src/ fix via a
// scratch-copied patch + `git checkout HEAD --` (NOT `git stash` — shared
// worktree refs/stash), rebuilt, confirmed T1/T2 SIGSEGV and T5 returns the
// untouched original value (202, the -54 no-op byte-masked); reapplied the
// byte-identical patch via `git apply`, rebuilt, confirmed all pass with the
// asserted values.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2400 {
    std::string path;
    explicit TempFile2400(std::string p) : path(std::move(p)) {}
    ~TempFile2400() { std::remove(path.c_str()); }
};

void writeSource2400(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2400(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object) -> exe, run it in a SEPARATE
// process (fork-isolated per tests/README.md's AOT harness discipline — a
// SIGSEGV in the run is contained to the child), return exit code (-1 on
// compile/link/run failure, -2 if the dependency itself could not be
// produced). Mirrors compileImportAndRun2387 / compileImportAndRun2380.
int compileImportAndRun2400(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2400_");
    TempFile2400 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2400 aObj((dir / (stem + "_a.o")).string());
    writeSource2400(aSrc.path, libSrc);
    if (!compileToObj2400(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2400 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2400 bExe((dir / stem).string());
    writeSource2400(bSrc.path, appSrc);
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
// T1 — the exact #2400 repro: a SETTER, inherited from an IMPORTED base
// class, satisfying a LOCAL interface's property obligation, accessed through
// the interface reference (itable dispatch). Pre-fix: SIGSEGV (null itable
// slot). Asserts the WRITTEN-THEN-READ VALUE (write 77, read back — a value
// distinguishable from both the backing field's initial value (1) and 0) so a
// coincidental exit code cannot masquerade as success.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_inherited_import_2400,
           setter_inherited_from_imported_base_local_iface_runs_and_dispatches) {
    int r = compileImportAndRun2400(
        "module BasePropLib2400T1; "
        "public class BaseWithProp2400T1 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end BaseWithProp2400T1;",
        "module App2400T1; import BasePropLib2400T1; "
        "public interface IHasName2400T1 "
        "  property Name: int; "
        "end IHasName2400T1; "
        "public class Derived2400T1: BaseWithProp2400T1, IHasName2400T1 "
        "end Derived2400T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2400T1 := new Derived2400T1(); "
        "    b.Name := 77; "
        "    return b.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 77);
}

// ===========================================================================
// T2 — TWO LEVELS of imported inheritance: the interface-satisfying settable
// property is declared on an imported base, inherited by a SECOND imported
// class (itself with no interface), which the LOCAL class then extends.
// Guards against a fix that only walks one level of the base chain. Asserts
// the round-trip value.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_inherited_import_2400,
           setter_inherited_two_levels_through_imported_bases_runs_and_dispatches) {
    int r = compileImportAndRun2400(
        "module BasePropLib2400T2; "
        "public class BaseWithProp2400T2 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end BaseWithProp2400T2; "
        "public class MiddleDerived2400T2: BaseWithProp2400T2 "
        "end MiddleDerived2400T2;",
        "module App2400T2; import BasePropLib2400T2; "
        "public interface IHasName2400T2 "
        "  property Name: int; "
        "end IHasName2400T2; "
        "public class Derived2400T2: MiddleDerived2400T2, IHasName2400T2 "
        "end Derived2400T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2400T2 := new Derived2400T2(); "
        "    b.Name := 88; "
        "    return b.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 88);
}

// ===========================================================================
// T3 — CONTROL: a settable property declared DIRECTLY on the (local)
// implementing class, not inherited, satisfying a LOCAL interface — no import
// involved in the property/interface relationship at all (the module still
// `import`s the base lib so the compiled program shape stays close to T1/T2,
// but Name is declared fresh on Derived2400T3, not inherited from the import).
// Guards against the fix breaking the ordinary, already-working local
// dispatch path (ensurePropertyAccessorFn must still take its normal
// `pd`-populated branch when the property genuinely IS local).
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_inherited_import_2400,
           setter_declared_locally_control_runs_and_dispatches) {
    int r = compileImportAndRun2400(
        "module UnrelatedLib2400T3; "
        "public class Unrelated2400T3 "
        "  public method Noop(): int => 0; "
        "end Unrelated2400T3;",
        "module App2400T3; import UnrelatedLib2400T3; "
        "public interface IHasName2400T3 "
        "  property Name: int; "
        "end IHasName2400T3; "
        "public class Derived2400T3: IHasName2400T3 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end Derived2400T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2400T3 := new Derived2400T3(); "
        "    b.Name := 99; "
        "    return b.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 99);
}

// ===========================================================================
// T4 — CONTROL: a settable property inherited from a SAME-MODULE (local) base
// class, satisfying a local interface. No import at all — mirrors #2387's T4,
// the same-module equivalent used to establish the crash was cross-module-
// specific. Drives the Driver directly (no dependency object needed).
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_inherited_import_2400,
           setter_inherited_same_module_control_runs_and_dispatches) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2400_t4_");
    TempFile2400 src((dir / (stem + ".bzl")).string());
    TempFile2400 exe((dir / stem).string());
    writeSource2400(src.path,
                    "module SameMod2400T4; "
                    "public class BaseWithPropSM2400T4 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    set => _n := value; "
                    "  end Name; "
                    "end BaseWithPropSM2400T4; "
                    "public interface IHasNameSM2400T4 "
                    "  property Name: int; "
                    "end IHasNameSM2400T4; "
                    "public class DerivedSM2400T4: BaseWithPropSM2400T4, "
                    "IHasNameSM2400T4 end DerivedSM2400T4; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b: IHasNameSM2400T4 := new DerivedSM2400T4(); "
                    "    b.Name := 66; "
                    "    return b.Name; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 66);
}

// ===========================================================================
// T5 — the DIRECT (non-itable) shape: `importedObj.Prop := v` where Prop is
// accessed through the imported class's OWN type, not an interface reference.
// No itable/vtable dispatch is involved at all — this exercises
// emitExportedPropertySetters / declareImportedPropertySetters /
// lowerImportedPropertySetValue on their own, independent of #2380/#2387's
// interface-obligation machinery. Pre-fix this did not crash; it silently
// dropped the write (confirmed by probe: returned the untouched original
// value 1, not 88). Asserts the written-then-read value with a discriminator
// distinct from the field's initializer.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_inherited_import_2400,
           direct_cross_module_set_no_interface_runs_and_writes) {
    int r = compileImportAndRun2400(
        "module BasePropLib2400T5; "
        "public class BaseWithProp2400T5 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end BaseWithProp2400T5;",
        "module App2400T5; import BasePropLib2400T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: BaseWithProp2400T5 := new BaseWithProp2400T5(); "
        "    d.Name := 88; "
        "    return d.Name; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 88);
}

}  // namespace bezel::module
