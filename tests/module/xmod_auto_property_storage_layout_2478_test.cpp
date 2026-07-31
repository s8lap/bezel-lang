// fix #2478 (§9.2/§22.8, v0.235): an imported class or record's
// AUTO-PROPERTY (`property X: T := v;` / `property X: T;` — backed by
// compiler-generated storage, §9.2) was never recognized as an instance
// STORAGE member on the importer side — regardless of visibility. §22.8's
// storage layout ("declaration order, not the §13 exported surface, drives
// physical layout") applies to an auto-property exactly as it does to a
// plain field (the precedent #2456 established), but the classification
// test itself blocked it.
//
// ROOT CAUSE: `sema::isAutoPropertySymbol` / `Emitter::isAutoProperty`
// (SymbolCollection.cpp / LlvmBackend_impl.hpp) both require
// `dynamic_cast<const ast::PropertyDecl*>(ps->decl)` to succeed. For EVERY
// imported PropertySymbol, `ps->decl` is null (ModuleLoader.cpp's
// `MetaMemberKind::Property`/`Indexer` reconstruction never sets it — there
// is no AST on the importer side) — so these predicates unconditionally
// return `false` for an imported auto-property, public or private alike.
// `collectInstanceStorage`/`collectRecordStorage` (LlvmBackend_impl.hpp)
// call this predicate to decide whether a member occupies a struct slot, so
// an imported type's auto-property was silently EXCLUDED from the
// importer's own understanding of the type's layout — while the
// EXPORTER's own compiled functions (property getters/setters, `bz$<Type>
// $init`, any ordinary method) correctly include it, since THEY are
// compiled with a real decl.
//
// CONFIRMED LIVE MEMORY-SAFETY HAZARD (class shape, same failure class as
// #2456): a locally-declared derived class's own field lands at the
// physical offset the exporter's real (larger) layout assigned to a LATER
// base member, aliasing the two. T1 below is the exact discovering repro.
// The record-shape twin has no aliasing (records don't inherit) but the
// SAME under-counting meant the auto-property was never given a struct
// slot at all on the importer side — T4 confirms this separately blocked
// #2467's own confirmed repro (`public property W: int := 42;` reading `0`
// after import) even for a fully PUBLIC member with no private-field
// involvement whatsoever.
//
// THE FIX: a new wire bit `MetaMember::isAutoProperty`
// (ModuleMetadata.hpp/.cpp, full six-site discipline — hash, copy-ctor,
// copy-assign, operator==, emit, read — mirroring #2400's `hasSetter` /
// #2415's `setterVisibility` precedent exactly), computed by the exporter
// to mirror `sema::isAutoPropertySymbol`'s own predicate (Auto bodyKind,
// non-indexer, non-static) so the wire classification and the local-decl
// classification agree by construction. Published via a new SemaContext
// side table (`importedPropertyIsAutoStorage`, mirroring
// `importedPropertyHasSetter`'s existing shape). A new Emitter helper,
// `isAutoPropertyStorage`, tries the LOCAL (decl-based) predicate first,
// then falls back to the wire-carried bit for a decl-null Property.
//
// SCOPE (narrow, deliberate — CLAUDE.md rule 4): `isAutoPropertyStorage`
// replaces the bare predicate at exactly THREE call sites, all in the
// storage-layout/member-address-resolution family:
//   - collectInstanceStorage / collectRecordStorage (LlvmBackend_impl.hpp)
//     — struct SIZE/offset computation (the T1/T4 hazard above).
//   - storageMemberOf (LlvmBackend_Layout.cpp) — the central GEP-address
//     resolution for an ordinary member read/write (fieldAddr/
//     implicitFieldAddr); without this, T4's `b.W` read `0` even AFTER
//     the layout-only fix, because the READ dispatch independently needed
//     the same fallback (confirmed by direct before/after testing during
//     this fix — the layout fix alone was necessary but not sufficient).
//   - makePropertyThunk's auto-property fast path (LlvmBackend_Layout.cpp)
//     — a property referenced as a function value (§27.6).
// An audit during this investigation found ~30 OTHER call sites of the
// bare (unfixed) predicates across definite assignment, ownership/
// disposal ARC, sendability, serialization eligibility, and pattern
// destructuring — each has an independent failure direction (some
// silent-wrong-data, some spurious-reject, at least one
// disposal-miscount) and its own regression-test burden; NONE of those
// are touched here and remain open, tracked in #2478's own issue text as
// follow-up work rather than silently folded into this fix.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2478 {
    std::string path;
    explicit TempFile2478(std::string p) : path(std::move(p)) {}
    ~TempFile2478() { std::remove(path.c_str()); }
};

void writeSource2478(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2478(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2456/#2464/#2467/#2468's own compileImportAndRun helpers.
int compileImportAndRun2478(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2478_" + std::to_string(std::rand());
    TempFile2478 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2478 aObj((dir / (stem + "_a.o")).string());
    writeSource2478(aSrc.path, libSrc);
    if (!compileToObj2478(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2478 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2478 bExe((dir / stem).string());
    writeSource2478(bSrc.path, appSrc);
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
// T1 — CORE repro (CLASS): a base class with a PUBLIC auto-property (`W`)
// followed by a PRIVATE plain field (`_own`, exposed via `Own`), and a
// LOCAL derived class with its OWN private field (`_derived`, exposed via
// `Derived`). ⚠ runAot's exit code is WEXITSTATUS-masked to 8 bits (lane
// protocol) — three DISTINCT primes (41, 43, 47) are used so that any
// two-way aliasing (either direction) produces a sum unreachable by the
// correct answer: pre-fix, `Own` and `Derived` both read `Derived`'s own
// value (47), so `W + Own + Derived` was 41+47+47=135; the reverse
// (`Derived` aliasing `Own`) would give 41+43+43=127. Only the fully
// independent reading gives 131.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           derived_field_does_not_alias_base_auto_property_9_2_22_8) {
    int r = compileImportAndRun2478(
        "module Lib2478T1; "
        "public class Base2478T1 "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T1;",
        "module App2478T1; import Lib2478T1; "
        "public class Derived2478T1: Base2478T1 "
        "  private _derived: int := 47; "
        "  public property Derived: int => _derived; "
        "end Derived2478T1; "
        "public class C2478T1 "
        "  public static method Main(): int "
        "    using var d := new Derived2478T1(); "
        "    return d.W + d.Own + d.Derived; "
        "  end Main; "
        "end C2478T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 131);  // 41 + 43 + 47, each read from its own slot
}

// ===========================================================================
// T2 — the individual reads of T1's shape in isolation, pinning exactly
// which values are independently correct (not just that the SUM is right —
// a coincidental sum could theoretically mask a two-way swap, though the
// distinct-prime choice above already rules that out; this test removes
// any doubt).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           derived_field_isolated_reads_all_independent_9_2_22_8) {
    int rw = compileImportAndRun2478(
        "module Lib2478T2; "
        "public class Base2478T2 "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T2;",
        "module App2478T2W; import Lib2478T2; "
        "public class Derived2478T2W: Base2478T2 "
        "  private _derived: int := 47; "
        "  public property Derived: int => _derived; "
        "end Derived2478T2W; "
        "public class C2478T2W "
        "  public static method Main(): int "
        "    using var d := new Derived2478T2W(); "
        "    return d.W; "
        "  end Main; "
        "end C2478T2W;");
    if (rw == -2) return;
    BEZEL_EXPECT_EQ(rw, 41);

    int ro = compileImportAndRun2478(
        "module Lib2478T2b; "
        "public class Base2478T2b "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T2b;",
        "module App2478T2O; import Lib2478T2b; "
        "public class Derived2478T2O: Base2478T2b "
        "  private _derived: int := 47; "
        "  public property Derived: int => _derived; "
        "end Derived2478T2O; "
        "public class C2478T2O "
        "  public static method Main(): int "
        "    using var d := new Derived2478T2O(); "
        "    return d.Own; "
        "  end Main; "
        "end C2478T2O;");
    if (ro == -2) return;
    BEZEL_EXPECT_EQ(ro, 43);

    int rd = compileImportAndRun2478(
        "module Lib2478T2c; "
        "public class Base2478T2c "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T2c;",
        "module App2478T2D; import Lib2478T2c; "
        "public class Derived2478T2D: Base2478T2c "
        "  private _derived: int := 47; "
        "  public property Derived: int => _derived; "
        "end Derived2478T2D; "
        "public class C2478T2D "
        "  public static method Main(): int "
        "    using var d := new Derived2478T2D(); "
        "    return d.Derived; "
        "  end Main; "
        "end C2478T2D;");
    if (rd == -2) return;
    BEZEL_EXPECT_EQ(rd, 47);
}

// ===========================================================================
// T3 — WRITE path: assigning through an INHERITED imported auto-property
// from a local derived class must not corrupt the derived class's own
// independent field (the write-side twin of T1's read-side check).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           write_to_inherited_auto_property_does_not_alias_9_2_22_8) {
    int r = compileImportAndRun2478(
        "module Lib2478T3; "
        "public class Base2478T3 "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T3;",
        "module App2478T3; import Lib2478T3; "
        "public class Derived2478T3: Base2478T3 "
        "  private _derived: int := 47; "
        "  public property Derived: int => _derived; "
        "end Derived2478T3; "
        "public class C2478T3 "
        "  public static method Main(): int "
        "    using var d := new Derived2478T3(); "
        "    d.W := 100; "
        "    return d.W + d.Own + d.Derived; "
        "  end Main; "
        "end C2478T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 190);  // 100 + 43 + 47 — write landed only in W's slot
}

// ===========================================================================
// T4 — CORE repro (RECORD): a fully PUBLIC record auto-property (no
// private-field involvement, no inheritance) — this is #2467's OWN
// confirmed repro shape, which stayed blocked by THIS defect even after
// #2467's own wire-carriage fix landed (that fix correctly threaded the
// VALUE across the boundary, but the auto-property never reached
// `buildRecordValue`'s per-field loop at all, since `collectRecordStorage`
// never gave it a storage slot). Combined with a positional field (using
// distinct values, addition per lane protocol) to also confirm the
// auto-property's slot doesn't collide with the positional field's.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           record_public_auto_property_gets_a_storage_slot_9_2) {
    int r = compileImportAndRun2478(
        "module Lib2478T4; "
        "public record Rec2478T4(x: int) "
        "  public property W: int := 41; "
        "end Rec2478T4;",
        "module App2478T4; import Lib2478T4; "
        "public class C2478T4 "
        "  public static method Main(): int "
        "    var r := new Rec2478T4(9); "
        "    var w := r.W; "
        "    return r.x + w; "
        "  end Main; "
        "end C2478T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 50);  // 9 + 41
}

// ===========================================================================
// T5 — index ARITHMETIC across MULTIPLE auto-properties (not just "off by
// one slot"): three public auto-properties on the base, each a distinct
// prime, all read through a local derived class holding its own field —
// mirrors #2456/#2468's own "multiple fields" grid entries.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           multiple_auto_properties_all_independent_9_2_22_8) {
    int r = compileImportAndRun2478(
        "module Lib2478T5; "
        "public class Base2478T5 "
        "  public property A: int := 2; "
        "  public property B: int := 3; "
        "  public property C: int := 5; "
        "end Base2478T5;",
        "module App2478T5; import Lib2478T5; "
        "public class Derived2478T5: Base2478T5 "
        "  private _own: int := 7; "
        "  public property Own: int => _own; "
        "end Derived2478T5; "
        "public class C2478T5 "
        "  public static method Main(): int "
        "    using var d := new Derived2478T5(); "
        "    return d.A + d.B + d.C + d.Own; "
        "  end Main; "
        "end C2478T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 17);  // 2 + 3 + 5 + 7, each field distinct
}

// ===========================================================================
// T6 — CONTROL: a fully LOCAL class (no import) with the identical shape as
// T1. Guards against this fix's new wire bit / SemaContext side table /
// `isAutoPropertyStorage` fallback affecting same-module compilation at
// all — a local symbol always has `ps->decl != nullptr`, so
// `isAutoPropertyStorage` should take the FIRST (unchanged, pre-existing
// `isAutoProperty`) branch every time.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           local_class_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2478_t6_" + std::to_string(std::rand());
    TempFile2478 src((dir / (stem + ".bzl")).string());
    TempFile2478 exe((dir / stem).string());
    writeSource2478(src.path,
                    "module LocalCtl2478T6; "
                    "public class LocalBase2478T6 "
                    "  public property W: int := 41; "
                    "  private _own: int := 43; "
                    "  public property Own: int => _own; "
                    "end LocalBase2478T6; "
                    "public class LocalDerived2478T6: LocalBase2478T6 "
                    "  private _derived: int := 47; "
                    "  public property Derived: int => _derived; "
                    "end LocalDerived2478T6; "
                    "public class C2478T6 "
                    "  public static method Main(): int "
                    "    using var d := new LocalDerived2478T6(); "
                    "    return d.W + d.Own + d.Derived; "
                    "  end Main; "
                    "end C2478T6;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 131);
}

// ===========================================================================
// T7 — CONTROL: DIRECT import construction (no local derivation) of a class
// with a public auto-property. This shape was already accidentally correct
// BEFORE this fix (confirmed by testing during this fix's development —
// no competing local subclass ever contended for the missing slot's
// offset, so the exporter's own self-consistent compiled functions
// happened to read/write correctly despite the importer's under-sized
// layout, per the latent-not-active hazard #2456's own T8 documents for
// the analogous private-field case). Guards against this fix regressing
// the one shape that already worked.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_storage_layout_2478,
           direct_import_construction_still_correct_control) {
    int r = compileImportAndRun2478(
        "module Lib2478T7; "
        "public class Base2478T7 "
        "  public property W: int := 41; "
        "  private _own: int := 43; "
        "  public property Own: int => _own; "
        "end Base2478T7;",
        "module App2478T7; import Lib2478T7; "
        "public class C2478T7 "
        "  public static method Main(): int "
        "    using var d := new Base2478T7(); "
        "    return d.W + d.Own; "
        "  end Main; "
        "end C2478T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 84);  // 41 + 43
}

}  // namespace bezel::module
