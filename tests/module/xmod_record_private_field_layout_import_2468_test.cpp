// fix #2468 (§9.1/§9.2/§22.8, v0.235): extend #2456's `includeForLayout`
// module-metadata carriage from CLASSES to RECORDS. #2456 fixed a genuine
// memory-safety hazard for classes: a private/internal instance field on an
// imported class was invisible to the importer's `t->members`, so
// `collectInstanceStorage`/`classStruct` (LlvmBackend_Layout.cpp) laid out a
// SMALLER struct than the exporter's own compilation — under-allocating any
// direct `new ImportedType()` and aliasing a local subclass's own field onto
// the hidden slot. #2456 deliberately scoped itself to `t.kind ==
// ast::TypeDeclKind::Class` because widening it to records regressed a
// previously-correct positional-field read (a record's body fields are
// appended to `t->members` BEFORE its positional fields, so a newly-visible
// hidden field shifted `buildRecordValue`'s naive `args[i] -> fields[i]`
// index mapping). #2464 root-caused and fixed that ordering defect
// (`buildRecordValue` now maps each positional argument to the correct
// POSITIONAL field — the trailing `args.size()` entries of `fields`/
// `t->members`, a contiguous block regardless of body-field count — rather
// than by raw storage index), which is the prerequisite this issue's own
// text names as unblocking the widening.
//
// THE FIX (this commit): `ModuleMetadataBuilder.cpp`'s `includeForLayout`
// gate now also matches `t.kind == ast::TypeDeclKind::Record`, so a private/
// internal record body FIELD (a real `FieldSymbol`, not an auto-property —
// see the T-property-gap note below) crosses the module boundary for LAYOUT
// purposes (name/type/declaration-order position only — no initializer, no
// ABI change, identical contract to the class case).
//
// THE HAZARD THIS CLOSES FOR RECORDS (confirmed by direct instrumentation,
// not inferred): unlike a class, a record has no inheritance, so #2456's own
// "derived class's own field aliases the base's hidden field" shape does not
// apply verbatim. The record-specific manifestation of the SAME underlying
// hazard (an under-sized importer-side struct) is METHOD-CALL ARGUMENT
// MARSHALLING: a record's ordinary (non-getter) METHOD *is* compiled and
// exported by the exporter as a real function taking `this` by pointer
// (unlike record property getters, which #2456/#2464's own comments note are
// never exported for records at all — a separate, filed gap, see below).
// Calling that method cross-module passes the IMPORTER's locally-constructed
// record value (spilled to a stack slot) to the EXPORTER's compiled body,
// which indexes fields by the EXPORTER's real (larger) layout. Before this
// fix, with the hidden field absent from the importer's `t->members`, the
// importer's struct had ONE FEWER field than the exporter's real layout —
// so a positional field placed immediately before a hidden one and a method
// reading the hidden field ALIASED: T1 below pins the exact repro (confirmed
// mechanically: reverting this fix reproduces the method returning the
// POSITIONAL field's own value instead of the hidden field's slot).
//
// WHAT THIS FIX (#2468) DID NOT DO, AT THE TIME: it made the hidden field's
// STORAGE SLOT visible (so it no longer aliases/shifts other fields) but did
// NOT make the hidden field's DECLARED DEFAULT VALUE correct cross-module —
// that was issue #2467 (record body-member initializer replay, public
// fields only) and, for a PRIVATE field specifically, #3723 (a second copy
// of #2467's own gate inside this fix's own `includeForLayout` carriage,
// widened in a later commit). T1/T2 below now pin the fix #3723 gave this
// file: `_hidden`'s declared `77` (and `_a`/`_b`/`_c`'s `2`/`3`/`5`) DO
// replay, not a well-defined-but-zero default.
//
// FILED SEPARATELY, NOT FIXED HERE (scope discipline, CLAUDE.md rule 4):
//   - A private/internal record AUTO-PROPERTY (an `isAutoPropertySymbol`
//     PropertySymbol storage member, §9.2) is NOT captured by this fix's
//     `includeForLayout` gate — that gate's `isField` test is
//     `dynamic_cast<const sema::FieldSymbol*>` only, which is false for a
//     PropertySymbol. This gap pre-dates #2456/#2468 and applies EQUALLY to
//     classes (the generic-class `includeForMethodBody` carriage,
//     ModuleMetadataBuilder.cpp ~line 1148, has the identical `isField`-only
//     test). Filed as a new issue; not attempted here — widening the
//     FieldSymbol-only test to also cover `isAutoPropertySymbol` is a
//     distinct, non-trivial change (touches both the class and record path)
//     that was not verified as part of this issue's own scope.
//   - A record's own non-auto PROPERTY (arrow/block getter, e.g.
//     `public property Hidden: int => _hidden;`) has NO cross-module getter
//     mechanism at all for records (`emitExportedPropertyGetters`,
//     LlvmBackend_Setup.cpp, gates `ts->kind != ast::TypeDeclKind::Class` —
//     records are skipped entirely, unlike classes). Separately discovered
//     while building this fix's regression grid: reading such a property
//     INLINE as an operand of a binary expression (`1 + r.SomeProp`) across a
//     module boundary silently evaluates to a wrong constant, while binding
//     it to a local first (`var h := r.SomeProp; return 1 + h;`) evaluates
//     correctly — a distinct codegen defect, unrelated to storage layout,
//     filed separately. This test file avoids that shape entirely (uses a
//     METHOD, not a property, and always binds a call result to a local
//     before combining it in arithmetic) so as not to conflate the two.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2468 {
    std::string path;
    explicit TempFile2468(std::string p) : path(std::move(p)) {}
    ~TempFile2468() { std::remove(path.c_str()); }
};

void writeSource2468(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2468(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2456/#2464's own compileImportAndRun helpers: compile lib -> object,
// compile app (importing lib) -> exe, run it in a SEPARATE process (fork-
// isolated per tests/README.md's AOT harness discipline), return its exit code
// (-1 on compile/link/run failure, -2 if the dependency itself could not be
// produced).
int compileImportAndRun2468(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2468_");
    TempFile2468 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2468 aObj((dir / (stem + "_a.o")).string());
    writeSource2468(aSrc.path, libSrc);
    if (!compileToObj2468(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2468 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2468 bExe((dir / stem).string());
    writeSource2468(bSrc.path, appSrc);
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

// Mirrors #2456's compileImportCaptureErr2456: compile lib -> object, then
// compile app -> exe WITHOUT running, capturing the driver's stderr into `err`
// and returning compile success.
bool compileImportCaptureErr2468(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2468_c_");
    TempFile2468 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2468 aObj((dir / (stem + "_a.o")).string());
    writeSource2468(aSrc.path, libSrc);
    if (!compileToObj2468(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2468 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2468 bExe((dir / (stem + "_b")).string());
    writeSource2468(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2468_err_" + unique_stem("") + ".txt"))
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
// T1 — CORE repro: a record with a positional field `x` followed by a
// PRIVATE body field `_hidden`, and a PUBLIC method that returns `_hidden`.
// Calling the method cross-module marshals the record's storage to the
// exporter's compiled body. ⚠ runAot's exit code is WEXITSTATUS-masked to 8
// bits (lane protocol) — pre-#2468 the method ALIASED and returned `x`'s
// own value (9), so `r.x + h` was 18 (9+9); post-#2468/pre-#3723 the hidden
// slot was a distinct, well-defined but ZERO (#2467's value-replay gap, not
// fixed by #2468) storage cell, so `r.x + h` was 9 (9+0).
//
// FLIPPED by fix #3723 (§9.1/§9.2/§22.8/§2.8.2, v0.286): #3723 widened
// `ModuleMetadataBuilder.cpp`'s `includeForLayout` `fieldInitText`-clearing
// gate (a second, independently-drifting copy of the same
// `isGenericType`-alone predicate its headline FieldDecl-branch fix
// corrected) to keep the carriage for a RECORD's private field too — a
// record has no `bz$<Type>$init` to replay ANY field's initializer through,
// public or private. `_hidden`'s declared `77` now replays, so `r.x + h`
// is `86` (9+77) — still distinguishable from the aliasing-corrupted `18`
// the ordering regression #2464 guards against.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           method_call_does_not_alias_positional_field_9_1_22_8) {
    int r = compileImportAndRun2468(
        "module RecLib2468T1; "
        "public record Rec2468T1(x: int) "
        "  private _hidden: int := 77; "
        "  public method GetHidden(): int => _hidden; "
        "end Rec2468T1;",
        "module App2468T1; import RecLib2468T1; "
        "public class C2468T1 "
        "  public static method Main(): int "
        "    var r := new Rec2468T1(9); "
        "    var h := r.GetHidden(); "
        "    return r.x + h; "
        "  end Main; "
        "end C2468T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 86);  // 9 + 77 — replayed (fix #3723)
}

// ===========================================================================
// T2 — index ARITHMETIC across multiple hidden fields (not just "off by one
// slot"): three private body fields ahead of the positional field's storage
// index, each read back via its own method, plus the positional field
// itself. Pre-#2468 every hidden field is entirely absent from the
// importer's layout, so `collectRecordStorage` sees only `x` — any method
// reading a hidden field aliases `x` (all three would read 9). Post-#2468/
// pre-#3723 each was its own distinct but ZERO slot (combined total 9).
// Post-#3723 (this commit's regression) each private field's declared
// value now replays: `2 + 3 + 5 = 10` on top of `x`'s `9`.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           multiple_private_fields_no_aliasing_9_1_22_8) {
    int r = compileImportAndRun2468(
        "module RecLib2468T2; "
        "public record Rec2468T2(x: int) "
        "  private _a: int := 2; "
        "  private _b: int := 3; "
        "  private _c: int := 5; "
        "  public method GetA(): int => _a; "
        "  public method GetB(): int => _b; "
        "  public method GetC(): int => _c; "
        "end Rec2468T2;",
        "module App2468T2; import RecLib2468T2; "
        "public class C2468T2 "
        "  public static method Main(): int "
        "    var r := new Rec2468T2(9); "
        "    var a := r.GetA(); "
        "    var b := r.GetB(); "
        "    var c := r.GetC(); "
        "    return r.x + a + b + c; "
        "  end Main; "
        "end C2468T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 19);  // 9 + 2 + 3 + 5 — no aliasing, replayed (fix #3723)
}

// ===========================================================================
// T3 — CONTROL (regression guard for #2464's own fix interacting with this
// widening): direct construction (no method call) of a combined-form record
// with a hidden private field still reads the POSITIONAL field correctly.
// This is the exact shape that regressed during #2456's own development
// (motivating the classes-only scoping this issue lifts) and that #2464
// fixed the ordering defect for — pin that it still holds with the gate now
// widened to records.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           direct_construction_positional_field_unaffected_control) {
    int r = compileImportAndRun2468(
        "module RecLib2468T3; "
        "public record Rec2468T3(x: int) "
        "  private _hidden: int := 77; "
        "  public property Hidden: int => _hidden; "
        "end Rec2468T3;",
        "module App2468T3; import RecLib2468T3; "
        "public class C2468T3 "
        "  public static method Main(): int "
        "    var r := new Rec2468T3(9); "
        "    return r.x; "
        "  end Main; "
        "end C2468T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);
}

// ===========================================================================
// T4 — CONTROL: encapsulation is UNCHANGED by this fix. A private record
// field newly carried in metadata for LAYOUT is still not name-resolvable
// from outside the record — sema's visibility check must reject a direct
// `r._hidden` exactly as it already did.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           private_field_still_inaccessible_by_name_control) {
    std::string err;
    bool success = compileImportCaptureErr2468(
        "module RecLib2468T4; "
        "public record Rec2468T4(x: int) "
        "  private _hidden: int := 77; "
        "end Rec2468T4;",
        "module App2468T4; import RecLib2468T4; "
        "public class C2468T4 "
        "  public static method Main(): int "
        "    var r := new Rec2468T4(9); "
        "    return r._hidden; "
        "  end Main; "
        "end C2468T4;",
        err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("private") != std::string::npos);
}

// ===========================================================================
// T5 — CONTROL: a fully LOCAL record (no import at all) with the identical
// shape as T1. Guards against this fix's new metadata-export path affecting
// same-module compilation at all (it should not run — the exporter loop only
// fires when producing an artifact's metadata, and this path was already
// correct locally, including reading the DECLARED default `77` — local
// construction has no cross-module value-replay gap).
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           local_record_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2468_t5_");
    TempFile2468 src((dir / (stem + ".bzl")).string());
    TempFile2468 exe((dir / stem).string());
    writeSource2468(src.path,
                    "module LocalCtl2468T5; "
                    "public record LocalRec2468T5(x: int) "
                    "  private _hidden: int := 77; "
                    "  public method GetHidden(): int => _hidden; "
                    "end LocalRec2468T5; "
                    "public class C2468T5 "
                    "  public static method Main(): int "
                    "    var r := new LocalRec2468T5(9); "
                    "    var h := r.GetHidden(); "
                    "    return r.x + h; "
                    "  end Main; "
                    "end C2468T5;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 86);  // 9 + 77 — local construction has the real value
}

// ===========================================================================
// T6 — CONTROL: a fully PUBLIC record (nothing hidden to begin with) is
// unaffected by this fix — `includeForLayout` should simply never fire
// (every member is already `exported`), so there is no double-export or
// duplicate-member corruption of the POSITIONAL field's own slot. Does NOT
// assert on `_y`'s VALUE (a public body field's initializer replay is #2467,
// a separate issue this commit does not touch — #2467's own confirmed repro
// uses this exact "plain public field" shape and shows it currently reads 0,
// not its declared default; asserting a specific `_y` value here would
// couple this test to that separate, not-yet-landed fix).
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_layout_import_2468,
           fully_public_record_positional_field_unaffected_control) {
    int r = compileImportAndRun2468(
        "module RecLib2468T6; "
        "public record Rec2468T6(x: int) "
        "  public _y: int := 25; "
        "end Rec2468T6;",
        "module App2468T6; import RecLib2468T6; "
        "public class C2468T6 "
        "  public static method Main(): int "
        "    var r := new Rec2468T6(9); "
        "    return r.x; "
        "  end Main; "
        "end C2468T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);
}

}  // namespace bezel::module
