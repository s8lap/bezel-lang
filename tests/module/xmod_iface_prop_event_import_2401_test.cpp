// fix #2401 (§12.1 / §11.2 / §23 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.235):
// `Emitter::declareVtables()` (`LlvmBackend_Setup.cpp`) Pass 0 assigns every
// LOCAL interface's methods, then its (sync) events, then its properties'
// getter/setter slots a running 0-based itable index — `ifaceMethodIndex_`,
// `ifaceEventIndex_`, `ifacePropertyGetIndex_`/`ifacePropertySetIndex_`. The
// adjacent "mirror this for IMPORTED interfaces too" block (added for
// #426/#328, so a cross-module interface CALL doesn't silently bail) only
// ever mirrored `ifaceMethodIndex_` — it never extended to events or
// properties, even though Pass 0's local loop populates all three from the
// exact same member list. A property or event resolved through a reference
// typed by an IMPORTED interface (`b: IHasName` where `IHasName` itself comes
// from an `import`, as opposed to #2387's shape — a LOCAL interface satisfied
// by an IMPORTED base class) therefore found no entry in
// `ifacePropertyGetIndex_`/`ifacePropertySetIndex_`/`ifaceEventIndex_` for
// that interface's `PropertySymbol`/`EventSymbol`.
//
// Unlike the METHOD case (#426/#328), which fails LOUD — every
// `ifaceMethodIndex_.find(ms)` call site gates and bails dispatch entirely —
// the property lookup sites (`lowerVirtualPropertyGet`/`-Set`,
// `LlvmBackend_Exprs.cpp`) each fall through to OTHER fallback paths that are
// ALSO unpopulated for an interface-parented member (no `PropertyDecl` body,
// no computed/block getter), so the read/write silently produces a
// default/placeholder value instead of failing to compile or crashing — SILENT
// WRONG DATA, worse than a fail-fast crash. Filed as another confirmed
// instance of #2374's audit pattern, distinct in shape from the `astNode`/
// `decl`-only-gate family: here the gap is "the import-mirror block was
// written for one member kind and never extended to its siblings," not a
// missing decl fallback.
//
// The fix mirrors Pass 0's own event-then-property loops verbatim into the
// import-mirror block, continuing the same running `idx` after the methods —
// exactly as Pass 0 does for local interfaces — so the importer computes the
// SAME per-(class×interface) itable index the exporter (or, for a purely
// local implementer of an imported interface as in every test below, the
// importer's OWN Pass D fill) assigns.
//
// CONFIRMED SYMPTOM (verified by probe before writing this test, then
// mechanically by reverting the src/ fix — scratch-copied to a patch, NOT
// `git stash` — and re-running this exact filter): the issue's own repro
// (T1 below) compiles clean and runs clean (exit 0, no crash) but returns
// `0` where the correct answer is `101` (`1 + 100`) both pre-fix; post-fix it
// returns the correct `101`. All expected constants below are kept < 256 —
// `runAot`'s (and a plain child-process) exit status is `WEXITSTATUS`-masked
// to 8 bits, so a value >= 256 can silently alias a wrong result to the
// "right" observed byte (a mistake this file's own author made once while
// probing #2401 and caught only by re-deriving the modulus).
//
// SCOPE / WHAT REMAINS BROKEN (probed, NOT fixed here, filed separately):
//   - SETTER, property declared LOCALLY on the implementing class (no
//     cross-module accessor export needed at all — unlike #2400's shape),
//     satisfying an IMPORTED interface's read-write property obligation:
//     STILL silently no-ops (T5 below, compile-only, mirrors #2387 T5's
//     documentation pattern). Root cause, confirmed by probe: this fix's own
//     mirror correctly widens `ifacePropertySetIndex_`'s reserved index
//     space, but `Emitter::propertyHasSetter(ps)` (LlvmBackend_impl.hpp) is
//     gated on `dynamic_cast<const ast::PropertyDecl*>(ps->decl)` with NO
//     fallback — and `ps->decl` is unconditionally null for ANY imported
//     symbol (ModuleLoader.cpp's `MetaMemberKind::Property` case: "the getter
//     BODY is not reconstructed here... there is no ast::PropertyDecl in the
//     importer"). Called on the IMPORTED INTERFACE's own read-write
//     `PropertySymbol` (to decide whether Pass 0/its mirror should reserve a
//     setter slot at all), `propertyHasSetter` unconditionally returns
//     `false` regardless of the interface's real (reconstructed-in-
//     `modifiers`, just not consulted) read-write shape, so NO setter slot is
//     ever reserved — a DIFFERENT `astNode`/`decl`-only-gate instance of
//     #2374's audit pattern, filed as its own issue (not #2401's filed root
//     cause, and not #2400's cross-module-export gap, since no export is
//     needed for a locally-implemented property).
//   - EVENT, `.Subscribe` through an IMPORTED interface's `event` member:
//     STILL silently no-ops (observed: exit 100, not the expected 110 — the
//     subscribe call compiles and runs but never reaches the real backing
//     `EventSource<T>`, so a later `Raise` has no observable effect). Root
//     cause, confirmed by probe/code-read: this is NOT reachable by this
//     fix's `ifaceEventIndex_` mirror at all — `ModuleLoader::convertMember`
//     (ModuleLoader.cpp) has NO `case MetaMemberKind::Event:` (the comment
//     there reads "Indexer / Event are part of the schema but deferred"), so
//     an imported type's (class OR interface) `event` member is never
//     reconstructed as an `EventSymbol` in the first place — there is no
//     symbol for the mirror to iterate over. A strictly deeper gap than
//     #2401's filed root cause (a missing SYMBOL, not a missing INDEX for an
//     existing symbol); filed as its own issue. Not exercised as a value-
//     asserting test here (no gtest case would newly PASS by this fix, and
//     pinning the current wrong value as an expectation would be backwards).
//
// Both of the above are left commented in code, not silently patched over —
// CLAUDE.md rule 4 / the run's "report further gate instances for #2374's
// table rather than fixing silently" directive.

#include <cstdio>
#include <cstdlib>
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

struct TempFile2401 {
    std::string path;
    explicit TempFile2401(std::string p) : path(std::move(p)) {}
    ~TempFile2401() { std::remove(path.c_str()); }
};

void writeSource2401(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2401(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object) -> exe, run it in a SEPARATE
// process (fork-isolated per tests/README.md's AOT harness discipline), and
// return its exit code (-1 on compile/link/run failure, -2 if the dependency
// itself could not be produced). Mirrors compileImportAndRun2387.
int compileImportAndRun2401(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2401_" + std::to_string(std::rand());
    TempFile2401 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2401 aObj((dir / (stem + "_a.o")).string());
    writeSource2401(aSrc.path, libSrc);
    if (!compileToObj2401(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2401 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2401 bExe((dir / stem).string());
    writeSource2401(bSrc.path, appSrc);
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
bool compileImportSucceeds2401(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2401_ok_" + std::to_string(std::rand());
    TempFile2401 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2401 aObj((dir / (stem + "_a.o")).string());
    writeSource2401(aSrc.path, libSrc);
    if (!compileToObj2401(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2401 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2401 bExe((dir / (stem + "_b")).string());
    writeSource2401(bSrc.path, appSrc);
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
// T1 — the exact #2401 repro: a GETTER read through a reference typed by an
// IMPORTED INTERFACE, implemented by a LOCAL class satisfying it via a
// property inherited from an IMPORTED BASE class. Pre-fix: silently returns
// 0. Asserts `Name + 100` (not just `Name`), so a coincidental zero return
// cannot masquerade as success.
// ===========================================================================
BEZEL_TEST(xmod_iface_prop_event_import_2401,
           getter_through_imported_interface_dispatches_correct_value) {
    int r = compileImportAndRun2401(
        "module BasePropLib2401T1; "
        "public class BaseWithProp2401T1 "
        "  public property Name: int => 1; "
        "end BaseWithProp2401T1; "
        "public interface IHasName2401T1 "
        "  property Name: int; "
        "end IHasName2401T1;",
        "module App2401T1; import BasePropLib2401T1; "
        "public class DerivedGood2401T1: BaseWithProp2401T1, IHasName2401T1 "
        "end DerivedGood2401T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2401T1 := new DerivedGood2401T1(); "
        "    return b.Name + 100; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 101);
}

// ===========================================================================
// T2 — combines #2387's "two levels of imported inheritance" shape with
// #2401's "reached through an IMPORTED interface" shape: the interface-
// satisfying property is declared on an imported base, inherited by a SECOND
// imported class, which the LOCAL class extends — and IHasName2401T2 (the
// interface through which the read happens) is ALSO imported. Guards against
// a fix that only widens the itable index space for a directly-declared
// imported interface property and mishandles the inherited-through-two-
// imported-levels shape.
// ===========================================================================
BEZEL_TEST(xmod_iface_prop_event_import_2401,
           getter_two_levels_imported_inheritance_through_imported_iface) {
    int r = compileImportAndRun2401(
        "module BasePropLib2401T2; "
        "public class BaseWithProp2401T2 "
        "  public property Name: int => 1; "
        "end BaseWithProp2401T2; "
        "public class MiddleDerived2401T2: BaseWithProp2401T2 "
        "end MiddleDerived2401T2; "
        "public interface IHasName2401T2 "
        "  property Name: int; "
        "end IHasName2401T2;",
        "module App2401T2; import BasePropLib2401T2; "
        "public class Derived2401T2: MiddleDerived2401T2, IHasName2401T2 "
        "end Derived2401T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasName2401T2 := new Derived2401T2(); "
        "    return b.Name + 200; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 201);
}

// ===========================================================================
// T3 — TWO SEPARATE imported interfaces implemented by one local class, read
// through each independently. Guards against a fix that only correctly
// widens the FIRST imported interface's itable index space in the mirror
// loop (each `TypeSymbol` in `ctx_.importedSymbols()` resets `idx` to 0 —
// this pins that the reset, and the per-interface map entries, stay correct
// across multiple imported interfaces in the same compilation).
// ===========================================================================
BEZEL_TEST(xmod_iface_prop_event_import_2401,
           getter_two_independent_imported_interfaces_both_dispatch) {
    int r = compileImportAndRun2401(
        "module TwoIfaceLib2401T3; "
        "public interface IHasFoo2401T3 "
        "  property Foo: int; "
        "end IHasFoo2401T3; "
        "public interface IHasBar2401T3 "
        "  property Bar: int; "
        "end IHasBar2401T3;",
        "module App2401T3; import TwoIfaceLib2401T3; "
        "public class Impl2401T3: IHasFoo2401T3, IHasBar2401T3 "
        "  public property Foo: int => 3; "
        "  public property Bar: int => 4; "
        "end Impl2401T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Impl2401T3(); "
        "    var bf: IHasFoo2401T3 := obj; "
        "    var bb: IHasBar2401T3 := obj; "
        "    return bf.Foo * 10 + bb.Bar + 60; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 94);  // 3*10 + 4 + 60
}

// ===========================================================================
// T4 — CONTROL: a LOCAL interface (no import at all) satisfied by a property
// inherited from a LOCAL base class, matching #2387 T4's control shape. Guards
// against the fix regressing ordinary same-module interface dispatch — the
// local Pass 0 loop this fix's mirror parallels is untouched, but this pins
// it explicitly for the property-through-interface-reference shape.
// ===========================================================================
BEZEL_TEST(xmod_iface_prop_event_import_2401,
           getter_local_interface_control_unaffected) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2401_t4_" + std::to_string(std::rand());
    TempFile2401 src((dir / (stem + ".bzl")).string());
    TempFile2401 exe((dir / stem).string());
    writeSource2401(src.path,
                    "module LocalCtl2401T4; "
                    "public class BaseWithPropL2401T4 "
                    "  public property Name: int => 1; "
                    "end BaseWithPropL2401T4; "
                    "public interface IHasNameL2401T4 "
                    "  property Name: int; "
                    "end IHasNameL2401T4; "
                    "public class DerivedL2401T4: BaseWithPropL2401T4, "
                    "IHasNameL2401T4 end DerivedL2401T4; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b: IHasNameL2401T4 := new DerivedL2401T4(); "
                    "    return b.Name + 100; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 101);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL (compiles only — see the file header for why the value
// is not asserted, mirroring #2387 T5's pattern): a SETTABLE property declared
// LOCALLY on the implementing class, satisfying an IMPORTED interface's
// read-write property obligation, still compiles clean. Does NOT assert the
// runtime value: `b.Name := 42; return b.Name;` currently still returns the
// property's ORIGINAL value (the set silently no-ops) — a separate,
// still-open `propertyHasSetter`-decl-gate bug (see file header), filed as
// its own issue rather than fixed here.
// ===========================================================================
BEZEL_TEST(xmod_iface_prop_event_import_2401,
           setter_through_imported_interface_still_compiles_accept_control) {
    bool ok = compileImportSucceeds2401(
        "module SetPropLib2401T5; "
        "public interface IHasNameSet2401T5 "
        "  property Name: int; "
        "end IHasNameSet2401T5;",
        "module App2401T5; import SetPropLib2401T5; "
        "public class DerivedGoodSet2401T5: IHasNameSet2401T5 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end DerivedGoodSet2401T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IHasNameSet2401T5 := new DerivedGoodSet2401T5(); "
        "    b.Name := 42; "
        "    return b.Name + 100; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(ok);
}

}  // namespace bezel::module
