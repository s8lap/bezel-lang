// fix #2456 (§22.8 / §2.8.2-§2.8.3, v0.235): a non-virtual property (or
// plain field) inherited from an IMPORTED base misread a LOCAL derived
// class's own first storage-member slot instead — whenever the base
// contributed at least one PRIVATE (or internal) instance field and the
// derived class declared any storage member of its own. Silent wrong data,
// no diagnostic, no crash — the same failure class as #2452, but a READ/
// WRITE-dispatch defect rather than a construction defect: it corrupts
// state even when construction is entirely correct, and survives any amount
// of correct field initialization.
//
// ROOT CAUSE (confirmed by direct instrumentation of `collectInstanceStorage`
// / `classStruct`, LlvmBackend_Layout.cpp, comparing the EXPORTER's own
// compiled struct layout against the IMPORTER's reconstruction of the same
// type): `ModuleMetadataBuilder.cpp`'s member-export loop
// (`isExportedMember`) drops EVERY private/internal member of a non-generic
// class from the wire metadata — including plain instance FIELDS, which
// (unlike methods) are not just part of the callable/nameable surface but
// also occupy a slot in the type's PHYSICAL instance layout regardless of
// visibility (§22.8's storage order is a property of DECLARATION, not of
// the §13 exported surface). `collectInstanceStorage`/`classStruct`
// (LlvmBackend_impl.hpp / LlvmBackend_Layout.cpp) walk an imported base's
// reconstructed `t->members` with NO visibility gate at all to lay out a
// LOCAL derived class's own fields — so a hidden private base field left
// the derived class's own storage starting ONE SLOT TOO EARLY, physically
// ALIASING the base's real (but importer-invisible) field. A *direct*
// `new ImportedBase()` was affected too, in a more severe way: with no
// derived class to "absorb" the missing slot, the reconstructed instance
// was allocated ONE SLOT TOO SMALL outright — a heap-buffer-overflow the
// moment the exporter's real (correctly-sized) init/ctor/accessor bodies
// wrote to the hidden field — though this repo's harness could not force a
// live crash from it mechanically (the overwriting store lives in the
// LIBRARY's non-ASan-instrumented object code, so LLVM ASan's compile-time
// shadow-memory checks — inserted only into instrumented code — never see
// it; `bezelc --sanitize=address` instruments only the single executable
// TU, and `-c` object outputs cannot be ASan-instrumented at all per the
// CLI's own documented scope). The under-allocation itself IS proven
// directly: instrumenting `classStruct` showed the importer's
// reconstruction of an imported class with one private field had
// `storageCount=0` where the exporter's own compilation of the SAME class
// had `storageCount=1` — a byte-for-byte layout mismatch between the two
// sides of the module boundary, independent of any getter/setter dispatch
// question. It was NOT a lookup-miss: `importedPropGetterFns_.find(ps)`
// (LlvmBackend_Exprs.cpp) was confirmed (also by instrumentation) to find
// and correctly CALL the exporter's real getter function every time — the
// callee just read the wrong physical slot because the CALLER's (importer's)
// struct layout never reserved space for the invisible base field.
//
// THE FIX (this commit): `ModuleMetadataBuilder.cpp`'s member-export loop
// now also includes a non-static, non-exported instance FIELD of a
// non-generic CLASS purely for layout — name, type, and declaration-order
// POSITION only (the constant-foldable initializer text `convertMember`
// would otherwise attach is explicitly stripped, so this is genuinely
// layout-only carriage, mirroring the "no initializer, no ABI change"
// contract the pre-existing generic-class `includeForMethodBody` case
// already documents for the identical reason). `ModuleLoader.cpp` needed NO
// changes: its `MetaMemberKind::Field` reconstruction already builds a real
// (visibility-tagged) `FieldSymbol` unconditionally once a `MetaMember`
// entry exists — the field simply never crossed the boundary before. No
// new metadata field/bit was invented: this widens an EXISTING, already-
// shipped mechanism (the generic-class private-field carriage) to a
// broader set of classes, not a new wire-format addition.
//
// SCOPE: CLASSES ONLY. A record's positional field is appended to
// `t->members` AFTER its body fields regardless of source order (confirmed
// by instrumentation) — widening this fix to records made a previously-
// CORRECT positional-field read return 0 once a hidden body field shifted
// the storage index it is written to (`buildRecordValue`'s positional-arg
// storing was not verified against this reordering). That is a real,
// separate defect in record construction, filed as #2464 rather than
// fixed here; this fix explicitly excludes records (`t.kind ==
// ast::TypeDeclKind::Class`) to avoid regressing the record case at all.
// T6 below pins that a record with a hidden field still reads its
// positional field correctly (the regression this fix must NOT reintroduce).

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

struct TempFile2456 {
    std::string path;
    explicit TempFile2456(std::string p) : path(std::move(p)) {}
    ~TempFile2456() { std::remove(path.c_str()); }
};

void writeSource2456(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2456(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2387/#2414/#2449/
// #2452's own compileImportAndRun helpers.
int compileImportAndRun2456(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2456_" + std::to_string(std::rand());
    TempFile2456 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2456 aObj((dir / (stem + "_a.o")).string());
    writeSource2456(aSrc.path, libSrc);
    if (!compileToObj2456(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2456 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2456 bExe((dir / stem).string());
    writeSource2456(bSrc.path, appSrc);
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

// Compile lib -> object, then compile app -> exe WITHOUT running, capturing
// the driver's stderr into `err` and returning compile success. Mirrors
// #2449's compileImportCaptureErr2449 (itself mirroring #2380/#1943/#2412).
bool compileImportCaptureErr2456(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2456_c_" + std::to_string(std::rand());
    TempFile2456 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2456 aObj((dir / (stem + "_a.o")).string());
    writeSource2456(aSrc.path, libSrc);
    if (!compileToObj2456(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2456 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2456 bExe((dir / (stem + "_b")).string());
    writeSource2456(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2456_err_" + std::to_string(std::rand()) + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

}  // namespace

// ===========================================================================
// T1 — GETTER aliasing, the core repro: an imported base with a PRIVATE
// field (accessed via an arrow-getter property `Val`) and a LOCAL derived
// class with its OWN private field (`_own`, via property `Own`). Reads BOTH
// properties and combines them so any cross-talk between the two slots
// fails the assertion. ⚠ runAot's exit code is WEXITSTATUS-masked to 8
// bits (lane protocol), so the combination is plain addition, not a
// place-value encoding: pre-fix, `Val` returned `_own`'s value (5) too, so
// `Val + Own` would be `10`, not the correct `47` — still fully
// distinguishing "both slots read `_own`'s value" from "each slot reads its
// own value," entirely within 0-255.
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           getter_does_not_alias_derived_own_field_22_8) {
    int r = compileImportAndRun2456(
        "module BaseLib2456T1; "
        "public class Base2456T1 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2456T1;",
        "module App2456T1; import BaseLib2456T1; "
        "public class Derived2456T1: Base2456T1 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2456T1; "
        "public class C2456T1 "
        "  public static method Main(): int "
        "    using var d := new Derived2456T1(); "
        "    return d.Val + d.Own; "
        "  end Main; "
        "end C2456T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 47);
}

// ===========================================================================
// T2 — SETTER aliasing (explicitly requested: the getter and setter slots
// can be separately broken, #2438's own lesson). Writes through the
// INHERITED read-write property (`d.Val := 77`) and confirms the derived
// class's own independent field is UNCHANGED (still 5) — pre-fix, the
// setter wrote into `_own`'s physical slot (confirmed mechanically:
// reverting this fix reproduces `d.Own` reading 77, the SET value, after
// this exact sequence).
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           setter_does_not_alias_derived_own_field_22_8) {
    int r = compileImportAndRun2456(
        "module BaseLib2456T2; "
        "public class Base2456T2 "
        "  private _v: int := 0; "
        "  public property Val: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end Base2456T2;",
        "module App2456T2; import BaseLib2456T2; "
        "public class Derived2456T2: Base2456T2 "
        "  private _own: int := 5; "
        "  public property Own: int => _own; "
        "end Derived2456T2; "
        "public class C2456T2 "
        "  public static method Main(): int "
        "    using var d := new Derived2456T2(); "
        "    d.Val := 77; "
        "    return d.Own; "
        "  end Main; "
        "end C2456T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// ===========================================================================
// T3 — the INVERSE direction: writing the DERIVED class's own field must
// not corrupt the (separately, independently constructed) inherited base
// field. Sets the derived property, then reads BOTH, proving the two slots
// are genuinely independent in BOTH directions (not just that T1/T2
// happened to probe the one direction that was broken). ⚠ addition only,
// to stay within the 8-bit masked exit code (see T1): pre-fix, aliasing
// would make `d.Val` also observe the `d.Own := 61` write, so `Val + Own`
// would be `122` (61+61) instead of the correct `70` (9+61).
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           derived_own_field_write_does_not_alias_base_field_22_8) {
    int r = compileImportAndRun2456(
        "module BaseLib2456T3; "
        "public class Base2456T3 "
        "  private _v: int := 9; "
        "  public property Val: int => _v; "
        "end Base2456T3;",
        "module App2456T3; import BaseLib2456T3; "
        "public class Derived2456T3: Base2456T3 "
        "  private _own: int; "
        "  public property Own: int "
        "    get => _own; "
        "    set => _own := value; "
        "  end; "
        "end Derived2456T3; "
        "public class C2456T3 "
        "  public static method Main(): int "
        "    using var d := new Derived2456T3(); "
        "    d.Own := 61; "
        "    return d.Val + d.Own; "
        "  end Main; "
        "end C2456T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 70);  // 9 + 61
}

// ===========================================================================
// T4 — MULTIPLE private base fields ahead of the derived class's own field:
// a stronger check of the index ARITHMETIC (not just "off by one slot")
// — four private base fields, each with a DISTINCT prime value, all read
// through their own properties, plus the derived class's own field. ⚠
// addition only (8-bit masked exit code, see T1) — every field distinct and
// small enough that the correct total (28) cannot arise from a partial /
// duplicated / skipped read of this five-field set by coincidence (the
// five values 2,3,5,7,11 sum to 28 only when each is counted exactly once;
// dropping or duplicating any one of them changes the total).
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           multiple_private_base_fields_all_correct_22_8) {
    int r = compileImportAndRun2456(
        "module BaseLib2456T4; "
        "public class Base2456T4 "
        "  private _a: int := 2; "
        "  private _b: int := 3; "
        "  private _c: int := 5; "
        "  private _d: int := 7; "
        "  public property A: int => _a; "
        "  public property B: int => _b; "
        "  public property C: int => _c; "
        "  public property D: int => _d; "
        "end Base2456T4;",
        "module App2456T4; import BaseLib2456T4; "
        "public class Derived2456T4: Base2456T4 "
        "  private _own: int := 11; "
        "  public property Own: int => _own; "
        "end Derived2456T4; "
        "public class C2456T4 "
        "  public static method Main(): int "
        "    using var d := new Derived2456T4(); "
        "    return d.A + d.B + d.C + d.D + d.Own; "
        "  end Main; "
        "end C2456T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 28);  // 2 + 3 + 5 + 7 + 11
}

// ===========================================================================
// T5 — CONTROL: encapsulation must be UNCHANGED by this fix. A private
// field newly carried in metadata for LAYOUT is still not name-resolvable
// from outside the declaring class — sema's visibility check must reject a
// direct `d._v` exactly as it already did (the field being present in
// `t->members` for layout must not make it independently ACCESSIBLE by
// name).
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           private_field_still_inaccessible_by_name_control) {
    std::string err;
    bool success = compileImportCaptureErr2456(
        "module BaseLib2456T5; "
        "public class Base2456T5 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2456T5;",
        "module App2456T5; import BaseLib2456T5; "
        "public class C2456T5 "
        "  public static method Main(): int "
        "    using var d := new Base2456T5(); "
        "    return d._v; "
        "  end Main; "
        "end C2456T5;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("private") != std::string::npos);
}

// ===========================================================================
// T6 — CONTROL (regression guard): a positional RECORD with a hidden
// private body field, imported. This exact shape REGRESSED during
// development of this fix (a record's positional field is stored AFTER its
// body fields in `t->members`'s declaration order, so widening the
// layout-carriage fix to records shifted the positional field's index and
// broke a previously-correct read) — the fix now explicitly excludes
// records (`t.kind == ast::TypeDeclKind::Class` in
// ModuleMetadataBuilder.cpp). This test pins that the positional field
// still reads correctly.
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           record_positional_field_unaffected_control) {
    int r = compileImportAndRun2456(
        "module RecLib2456T6; "
        "public record Rec2456T6(x: int) "
        "  private _hidden: int := 77; "
        "  public property Hidden: int => _hidden; "
        "end Rec2456T6;",
        "module App2456T6; import RecLib2456T6; "
        "public class C2456T6 "
        "  public static method Main(): int "
        "    var r := new Rec2456T6(9); "
        "    return r.x; "
        "  end Main; "
        "end C2456T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);
}

// ===========================================================================
// T7 — CONTROL: a fully LOCAL base+derived pair (no import at all) with the
// identical shape as T1. Guards against this fix's new metadata-export path
// affecting same-module compilation at all (it should not run — the
// exporter loop only fires when producing an artifact's metadata, and this
// path was ALREADY correct locally).
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           local_base_and_derived_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2456_t7_" + std::to_string(std::rand());
    TempFile2456 src((dir / (stem + ".bzl")).string());
    TempFile2456 exe((dir / stem).string());
    writeSource2456(src.path,
                    "module LocalCtl2456T7; "
                    "public class LocalBase2456T7 "
                    "  private _v: int := 42; "
                    "  public property Val: int => _v; "
                    "end LocalBase2456T7; "
                    "public class LocalDerived2456T7: LocalBase2456T7 "
                    "  private _own: int := 5; "
                    "  public property Own: int => _own; "
                    "end LocalDerived2456T7; "
                    "public class C2456T7 "
                    "  public static method Main(): int "
                    "    using var d := new LocalDerived2456T7(); "
                    "    return d.Val + d.Own; "
                    "  end Main; "
                    "end C2456T7;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 47);
}

// ===========================================================================
// T8 — CONTROL: a *direct* `new ImportedBase()` (no local derivation) with a
// private field — the shape that was ALREADY correct before this fix (the
// under-allocation risk described in this file's header, not exercised by a
// live crash here, but the READ VALUE must stay correct either way). Guards
// against this fix regressing the one shape that already worked.
// ===========================================================================
BEZEL_TEST(xmod_private_field_layout_import_2456,
           direct_import_construction_still_correct_control) {
    int r = compileImportAndRun2456(
        "module BaseLib2456T8; "
        "public class Base2456T8 "
        "  private _v: int := 42; "
        "  public property Val: int => _v; "
        "end Base2456T8;",
        "module App2456T8; import BaseLib2456T8; "
        "public class C2456T8 "
        "  public static method Main(): int "
        "    using var d := new Base2456T8(); "
        "    return d.Val; "
        "  end Main; "
        "end C2456T8;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
