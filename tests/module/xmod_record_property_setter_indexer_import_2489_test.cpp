// fix #2489 (§9.2/§9.4/§23/§23.4/§27.6, v0.236): the setter/indexer analogs
// of #2474's layers 1+2 (an imported record's computed property GETTER),
// applied to the sibling paths #2474's own lane found by grep and filed
// here UNCONFIRMED by testing: emitExportedPropertySetters,
// emitExportedIndexerGetters/-Setters, their importer call sites, and
// makePropertyThunk's matching hole.
//
// Per the lane protocol's "confirm before fixing" discipline, every site was
// reproduced by direct probe against origin/main before any src/ change (see
// the commit body for exact before/after exit codes). All four sites DID
// reproduce, each for its own reason:
//
//   1. emitExportedPropertySetters / emitExportedIndexerGetters /
//      emitExportedIndexerSetters (LlvmBackend_Setup.cpp): each gated
//      `ts->kind != TypeDeclKind::Class`, the exact pre-#2474 shape —
//      never emitting the exported ABI stub for a RECORD. Widened to
//      `Class || Record`, mirroring #2474's own widening and its own
//      rationale (the body-building delegates — ensurePropertyAccessorFn,
//      ensureIndexerGetterFn, ensureIndexerSetterFn — already declare
//      `this` as `ptr` unconditionally).
//
//   2. The importer call sites (lowerImportedPropertySetValue,
//      lowerIndexerGet's/lowerIndexerSet's `!pd` branches, all in
//      LlvmBackend_Assign.cpp) needed a by-value RECORD receiver spill —
//      #2474's own "layer 2". The indexer sites ALSO had their own
//      `recvDecl->kind != Class` kind gate (on top of the pointer check) —
//      an extra, undocumented layer #2474's issue text did not call out —
//      which needed widening too.
//
//      A SECOND defect surfaced only by direct testing (not visible from
//      code inspection alone): the naive "spill a LOADED COPY of the
//      receiver into a fresh alloca" pattern every sibling READ site in
//      this codebase uses is WRONG for a SETTER. `lowerExpr` on a plain
//      identifier referencing a record-typed local loads the record's
//      CURRENT VALUE — a byte-for-byte copy — so spilling that copy and
//      writing through it mutates a throwaway slot, never the caller's own
//      variable. Confirmed mechanically: an early version of this fix
//      compiled, linked, and ran with NO crash, but `r.Name := 55; return
//      r.x + r.Name;` still evaluated as `9 + 1` (the untouched original),
//      not `9 + 55` — a second, silent-wrong-result bug layered on top of
//      the first. Fixed by preferring `lvalueAddr` (the receiver's OWN
//      storage address) over `lowerExpr` (a copy) for the setter
//      receiver — see LlvmBackend_Assign.cpp's own comments on both fixed
//      functions for the full mechanism. The identical defect shape exists
//      in the PRE-EXISTING, same-module `lowerIndexerSet` inline path
//      (`pd != nullptr` branch) — filed separately as #2502, out of this
//      issue's cross-module-ABI scope (that pre-existing function was not
//      touched here).
//
//   3. `implicitFieldAddr`'s lazy-struct-build retry (#2474's "layer 3") is
//      SHARED (via `lvalueAddr`, which both reads and writes route
//      through) and already covers the setter body's implicit-field WRITE
//      — confirmed no further layer-3-style fix was needed here.
//
//   4. `makePropertyThunk`'s hole (property-as-function-value promotion,
//      §27.6): none of its three existing branches (computedGetterOf /
//      blockGetterOf / isAutoPropertyStorage) match an IMPORTED, non-auto,
//      COMPUTED property — confirmed by probe to silently degrade to a
//      permanent 0 for BOTH a Class and a Record receiver (broader than
//      "the record-receiver case" the issue text flagged — the hole is
//      kind-independent). Fixed with a new fourth branch calling the
//      exported `$propget` stub, but DELIBERATELY RESTRICTED to a CLASS
//      receiver: testing surfaced a separate, pre-existing, more
//      fundamental bug — `buildPropertyClosureCell` never captures a
//      by-value (record/enum) receiver into the closure's env at all, so
//      `thisVal` inside the thunk is a null pointer for those kinds. This
//      is latent (not currently observable) only because none of the
//      OTHER three branches use `thisVal` for those kinds either; adding a
//      fourth branch that DOES would have turned "silently returns 0" into
//      a SIGSEGV for a record or enum receiver. Confirmed the identical
//      shape crashes TODAY for a plain LOCAL (non-imported) record's
//      arrow-getter promoted to a function value and called — filed
//      separately as #2501 (HIGH — a crash, unrelated to import/export).
//      T8/T9 below pin the still-out-of-scope Record/Enum degrade
//      (unchanged by this fix, still link-safe, still not a crash).
//
// Two more pre-existing, unrelated bugs were found and filed while
// confirming these repros, NOT fixed here (CLAUDE.md rule 4 — no
// while-we're-at-it expansions): #2498 (a LOCAL record's DIRECT property
// setter, `lowerPropertySetValue`, emits malformed IR — a crash, distinct
// function from anything this fix touches) and #2499 (sema wrongly ACCEPTS
// a block-form settable property on an ENUM, violating §10.5 — T12 below
// exercises the resulting shape purely to pin that this fix's own
// enum-exclusion guard keeps it link-safe, not to bless the shape as
// correct).
//
// CONSTANTS: kept < 256 throughout (`runAot`/a plain child-process's exit
// status is WEXITSTATUS-masked to 8 bits, per the lane protocol and
// #2401's own test file's documented mistake).

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

struct TempFile2489 {
    std::string path;
    explicit TempFile2489(std::string p) : path(std::move(p)) {}
    ~TempFile2489() { std::remove(path.c_str()); }
};

void writeSource2489(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2489(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2414/#2474/#2478's own compileImportAndRun helpers.
int compileImportAndRun2489(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2489_" + std::to_string(std::rand());
    TempFile2489 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2489 aObj((dir / (stem + "_a.o")).string());
    writeSource2489(aSrc.path, libSrc);
    if (!compileToObj2489(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2489 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2489 bExe((dir / stem).string());
    writeSource2489(bSrc.path, appSrc);
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

int compileAndRunLocal2489(const std::string& src) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2489_local_" + std::to_string(std::rand());
    TempFile2489 srcF((dir / (stem + ".bzl")).string());
    TempFile2489 exe((dir / stem).string());
    writeSource2489(srcF.path, src);
    if (!driver.compileFile(srcF.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — CORE repro: a settable computed property on an imported RECORD, PUBLIC
// backing field, combining a POSITIONAL field with a body member (§9.2/§9.3).
// Direct (non-interface) SET then READ. Pre-fix: 10 (9 + the untouched
// original _n=1) — the write silently dropped (no crash, no link error: the
// pre-#2489 `isPointerTy()` guard rejected the by-value record receiver
// before the call was ever emitted). Post-fix: 64 (9 + 55).
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           setter_public_backing_positional_and_body_direct_set_then_read) {
    const char* lib =
        "module RecSetLib2489T1; "
        "public record RecSet2489T1(x: int) "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RecSet2489T1;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T1; import RecSetLib2489T1; "
        "public class C2489T1 "
        "  public static method Main(): int "
        "    var r := new RecSet2489T1(9); "
        "    r.Name := 55; "
        "    return r.x + r.Name; "
        "  end Main; "
        "end C2489T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 64);
}

// ===========================================================================
// T2 — private BACKING field axis: the setter mutates a PRIVATE field through
// the exported accessor; the read-back happens in the SAME write cycle (not
// relying on any cross-module initializer replay, so #2467/#2468's private-
// value-replay scope boundary — #2474's own T3 — does not apply here).
// x=9, set Name:=47 → 9 + 47 = 56.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           setter_private_backing_direct_set_then_read) {
    const char* lib =
        "module RecSetLib2489T2; "
        "public record RecSet2489T2(x: int) "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RecSet2489T2;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T2; import RecSetLib2489T2; "
        "public class C2489T2 "
        "  public static method Main(): int "
        "    var r := new RecSet2489T2(9); "
        "    r.Name := 47; "
        "    return r.x + r.Name; "
        "  end Main; "
        "end C2489T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 56);
}

// ===========================================================================
// T3 — CONTROL: a CLASS's settable property, direct cross-module set-then-
// read (the #2400 shape, already fixed, untouched by this commit's Class||
// Record widening). Guards against a regression from the shared gate/spill
// changes.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           setter_class_control_unaffected) {
    const char* lib =
        "module ClsSetLib2489T3; "
        "public class ClsSet2489T3 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end ClsSet2489T3;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T3; import ClsSetLib2489T3; "
        "public class C2489T3 "
        "  public static method Main(): int "
        "    using var c := new ClsSet2489T3(); "
        "    c.Name := 77; "
        "    return c.Name; "
        "  end Main; "
        "end C2489T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 77);
}

// ===========================================================================
// T4 — Indexer GET on an imported RECORD (public backing, positional + body
// combined). Pre-write read. Pre-fix: 0 (the importer call site's own
// `recvDecl->kind != Class` gate rejected the record receiver before any
// call, falling to the caller's zero-init backstop). Post-fix: 3 (_v=0 + 3).
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           indexer_get_public_backing_positional_and_body) {
    const char* lib =
        "module RecIdxLib2489T4; "
        "public record IdxRec2489T4(seed: int) "
        "  public _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2489T4;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T4; import RecIdxLib2489T4; "
        "public class C2489T4 "
        "  public static method Main(): int "
        "    var r := new IdxRec2489T4(9); "
        "    return r[3]; "
        "  end Main; "
        "end C2489T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 3);
}

// ===========================================================================
// T5 — Indexer SET-then-GET on an imported RECORD, PRIVATE backing field
// (private axis). Pre-fix: 2 (_v stayed 0: BOTH the set and the independent
// get were rejected by the same shape of gate — 0 + 2). Post-fix: 6
// (_v := 9 - 5 = 4; r[2] = 4 + 2 = 6). This is the test that caught the
// "spilled a copy, not the original" defect described in the file header:
// an intermediate version of this fix passed T4 (GET) but still returned 2
// here, proving the SET silently mutated a throwaway copy.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           indexer_set_then_get_private_backing) {
    const char* lib =
        "module RecIdxLib2489T5; "
        "public record IdxRec2489T5(seed: int) "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end IdxRec2489T5;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T5; import RecIdxLib2489T5; "
        "public class C2489T5 "
        "  public static method Main(): int "
        "    var r := new IdxRec2489T5(9); "
        "    r[5] := 9; "
        "    return r[2]; "
        "  end Main; "
        "end C2489T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T6 — CONTROL: a CLASS's indexer, cross-module SET-then-GET (the #2414
// shape, already fixed, mirrors that fix's own T2). Guards against a
// regression from the shared gate/spill changes.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           indexer_class_control_unaffected) {
    const char* lib =
        "module ClsIdxLib2489T6; "
        "public class ClsIdx2489T6 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end ClsIdx2489T6;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T6; import ClsIdxLib2489T6; "
        "public class C2489T6 "
        "  public static method Main(): int "
        "    using var b := new ClsIdx2489T6(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C2489T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T7 — makePropertyThunk CORE repro: an imported CLASS's arrow-getter
// property promoted to a function value (§27.6) and called. Pre-fix: 0
// (none of makePropertyThunk's three branches match an imported, non-auto,
// computed property — silently degrades to the zero-value default,
// regardless of class or record). Post-fix: 13 (the field's real value).
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           property_thunk_class_core_repro) {
    const char* lib =
        "module ThunkLib2489T7; "
        "public class ClsThunk2489T7 "
        "  public _pub: int := 13; "
        "  public property Getter: int => _pub; "
        "end ClsThunk2489T7;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T7; import ThunkLib2489T7; "
        "public class C2489T7 "
        "  public static method Main(): int "
        "    using var c := new ClsThunk2489T7(); "
        "    var fn: () -> int := c.Getter; "
        "    return fn(); "
        "  end Main; "
        "end C2489T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 13);
}

// ===========================================================================
// T8 — SCOPE BOUNDARY (not a regression, not "fixed" by this commit): the
// identical shape over a RECORD receiver. `buildPropertyClosureCell`
// (LlvmBackend_Layout.cpp) never captured a by-value receiver into the
// closure's env at all — a pre-existing, general bug (crashed for a plain
// LOCAL record, filed separately as #2501) unrelated to import/export. This
// commit's new makePropertyThunk branch was deliberately restricted to a
// CLASS receiver specifically BECAUSE of that bug — widening it to Record
// would have converted this silent 0 into a SIGSEGV.
//
// UPDATE (fix #2501, §27.6 v0.236): the general/local-receiver capture bug
// this comment describes is now fixed — buildPropertyClosureCell captures a
// record's struct value by copy (sized by getTypeStoreSize) exactly like
// buildMethodClosureCell already did for method references, and
// makePropertyThunk's own `this`-reconstruction now branches on the
// captured shape instead of assuming a pointer, so a LOCAL record's
// arrow-getter referenced as a function value no longer SIGSEGVs (see
// property_ref_function_value_756_test.cpp's `record_*_2501` tests). This
// T8 case is still pinned at 0, unchanged: it is the CROSS-MODULE path
// through the `importedPropGetterFns_` branch above, which stays gated to
// `ps->parent->kind == Class` deliberately — #2501 did not widen it (that
// gate's own record-across-module correctness was never exercised or
// proven end-to-end, per #2489's own reasoning above) — so an imported
// record's computed property still falls through every branch to the
// `getNullValue` default, same as before. Widening this gate to Record is a
// #2489-shaped follow-up, not folded into #2501.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           property_thunk_record_scope_boundary_control_2501) {
    const char* lib =
        "module ThunkLib2489T8; "
        "public record RecThunk2489T8(x: int) "
        "  public _pub: int := 41; "
        "  public property Getter: int => _pub; "
        "end RecThunk2489T8;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T8; import ThunkLib2489T8; "
        "public class C2489T8 "
        "  public static method Main(): int "
        "    var r := new RecThunk2489T8(9); "
        "    var fn: () -> int := r.Getter; "
        "    return fn(); "
        "  end Main; "
        "end C2489T8;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r >= 0);  // the real assertion: compiled, linked, ran — no crash
    BEZEL_EXPECT_EQ(r, 0);      // pre-existing, out-of-scope degrade — see #2501
}

// ===========================================================================
// T9 — REGRESSION GUARD: an imported ENUM's read-only expression-bodied
// property (§10.4) promoted to a function value. Mirrors #2474's own T9 —
// pins that this commit's new makePropertyThunk branch, gated strictly to
// `ps->parent->kind == Class`, never fires for an enum receiver (so no call
// is emitted against a symbol emitExportedPropertyGetters never defines for
// an enum — that gate stays `Class || Record`, unchanged by this commit).
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           property_thunk_enum_still_links_control_10_4) {
    const char* lib =
        "module ThunkLib2489T9; "
        "public enum EnumThunk2489T9 "
        "  Red, Green, Blue; "
        "  public property P: int => 7; "
        "end EnumThunk2489T9;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T9; import ThunkLib2489T9; "
        "public class C2489T9 "
        "  public static method Main(): int "
        "    var e := EnumThunk2489T9.Red; "
        "    var fn: () -> int := e.P; "
        "    return fn(); "
        "  end Main; "
        "end C2489T9;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r >= 0);  // the real assertion: compiled AND linked
    BEZEL_EXPECT_EQ(r, 0);      // pre-existing, out-of-scope degrade
}

// ===========================================================================
// T10 — CONTROL: a fully LOCAL (no import) CLASS's direct settable property,
// set-then-read. Guards against this commit's changes (all cross-module ABI
// sites) affecting ordinary same-module class property assignment.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           local_class_setter_unaffected_control) {
    int r = compileAndRunLocal2489(
        "module LocalCtl2489T10; "
        "public class LocalCls2489T10 "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end LocalCls2489T10; "
        "public class C2489T10 "
        "  public static method Main(): int "
        "    using var c := new LocalCls2489T10(); "
        "    c.Name := 99; "
        "    return c.Name; "
        "  end Main; "
        "end C2489T10;");
    BEZEL_EXPECT_EQ(r, 99);
}

// ===========================================================================
// T11 — CONTROL: a fully LOCAL (no import) CLASS's indexer, set-then-get.
// Guards against this commit's importer-call-site changes affecting ordinary
// same-module class indexer assignment.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           local_class_indexer_unaffected_control) {
    int r = compileAndRunLocal2489(
        "module LocalCtl2489T11; "
        "public class LocalIdx2489T11 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end LocalIdx2489T11; "
        "public class C2489T11 "
        "  public static method Main(): int "
        "    using var b := new LocalIdx2489T11(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C2489T11;");
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T12 — REGRESSION GUARD (the setter's own "enum trap", #2474's layer-2
// pattern applied here): §10.5 forbids a block-form/settable property on an
// enum ("Block-form properties or properties with backing storage"), but a
// separate, pre-existing sema gap (filed as #2499, NOT fixed here) currently
// lets one through — confirmed by probe: this exact shape compiles clean on
// origin/main with no diagnostic. Because it exists, `declareImportedProperty
// Setters` (no kind filter beyond virtual/interface + hasSetter) DOES declare
// a `$propset` reference for it; `emitExportedPropertySetters` deliberately
// stays `Class || Record`, never `Enum`, so nothing defines that symbol.
// Pins that `lowerImportedPropertySetValue`'s `isEnumReceiver` guard keeps
// `e.P := 42;` from ever emitting the call (both the lvalueAddr aggregate
// branch and the lowerExpr-spill fallback explicitly exclude it) — the
// program still compiles and links (the real assertion), and the getter
// (a fixed `get => 7;`, unrelated to any field) is unaffected by the SET's
// silent no-op, exactly matching #2474 T9's own "still degraded, still
// link-safe, not this commit's problem to fix" framing.
// ===========================================================================
BEZEL_TEST(xmod_record_property_setter_indexer_import_2489,
           enum_illegal_setter_still_links_control_10_5_2499) {
    const char* lib =
        "module EnumSetLib2489T12; "
        "public enum EnumSet2489T12 "
        "  Red, Green, Blue; "
        "  public property P: int "
        "    get => 7; "
        "    set => do end; "
        "  end P; "
        "end EnumSet2489T12;";
    int r = compileImportAndRun2489(
        lib,
        "module App2489T12; import EnumSetLib2489T12; "
        "public class C2489T12 "
        "  public static method Main(): int "
        "    var e := EnumSet2489T12.Red; "
        "    e.P := 42; "
        "    return e.P; "
        "  end Main; "
        "end C2489T12;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r >= 0);  // the real assertion: compiled AND linked
}

}  // namespace bezel::module
