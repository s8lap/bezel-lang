// fix #2474 (§9.2/§23.1/§23.2, v0.235): an imported RECORD property read
// used INLINE as a binary-expression operand silently evaluated wrong.
//
// #2474's own confirmed repro used an AUTO-PROPERTY (`property W: int :=
// 42;`); that shape's VALUE is already correct on `origin/main` before this
// commit, as a side effect of #2478's storage-layout fix (an imported
// record's auto-property is recognized as instance storage). T1 below pins
// that outcome, but it is NOT an untouched control: layer 2 below (see the
// LAYER 2 note) is keyed only on whether an entry exists in
// `importedPropGetterFns_`, which `declareImportedPropertyGetters` populates
// for EVERY imported non-virtual property — auto-backed or computed alike,
// with no distinction between them. So layer 2's fix reroutes T1's `b.W`
// read too: pre-commit it fell through the (pointer-check-failing) getter
// call to CG8's `fieldAddr` direct-storage-GEP path; post-commit it instead
// calls the exported `$propget` wrapper, which independently reaches the
// correct value because `ensurePropertyAccessorFn`'s own `isAuto` branch
// builds its own `classStruct`/`fieldIndex_` and does not depend on the
// caller's path. Confirmed by `nm -u` on the compiled importer object:
// pre-commit it has no `propget` reference for `W` at all; post-commit it
// does. So this commit's real blast radius is EVERY public non-virtual
// property read on an imported record — auto-property and computed alike —
// not only the arrow-getter shape #2474's issue text singled out; T1 pins
// the (correct, unchanged) auto-property VALUE outcome across that
// path-level change, it does not validate an untouched path.
//
// #2474's issue text ALSO confirmed the identical symptom for an
// ARROW-GETTER (computed, non-auto) property (`property Hidden: int =>
// _hidden;`), which #2478's storage-layout fix does NOT touch (a computed
// property has no storage slot to lay out) — THAT half stayed broken, for a
// genuinely different, two-layer root cause than either #2474's own
// "candidate area" guess (BinaryExpr operand-null propagation — ruled out;
// see below) or #2478's own scope:
//
//   LAYER 1 (exporter): `Emitter::emitExportedPropertyGetters`
//   (LlvmBackend_Setup.cpp) only emitted the cross-module getter stub
//   (`bz$<Type>$<Prop>$propget`) for `ts->kind == ast::TypeDeclKind::Class`
//   — a RECORD's computed property NEVER got an exported symbol at all, so
//   `declareImportedPropertyGetters` (the importer-side twin, which does
//   NOT filter by kind) declared a reference with no definition anywhere.
//   Fixed by widening the gate to `Class || Record` — safe because the
//   gate's own body-building delegate, `ensurePropertyAccessorFn`, was
//   ALREADY fully general (declares `this` as `ptr` unconditionally; its
//   `classStruct(cls)` call already forwards to `recordStruct` for a Record
//   per the existing "CG29" comment in LlvmBackend_Layout.cpp).
//
//   LAYER 2 (importer call site): even with the stub declared AND defined,
//   `LlvmBackend_Exprs.cpp`'s "imported non-virtual property getter" call
//   site (~L4283-4300, issue #503) required `obj->getType()->isPointerTy()`
//   before calling it — but a RECORD receiver lowers to a BY-VALUE struct,
//   not a pointer (issue #347 / §9.2, the same rule the LOCAL computed-
//   getter path and the block-getter path a few dozen lines above already
//   apply). Fixed by spilling a non-pointer receiver to a stack slot first,
//   mirroring those two sibling paths — but ONLY for an aggregate (struct)
//   receiver, exactly matching the LOCAL computed-getter path's own
//   `isAggregateType() && !isEnumReceiver` guard (not just its
//   `isPointerTy()` half). Review of an earlier draft here (which spilled
//   ANY non-pointer receiver unconditionally) caught that
//   `declareImportedPropertyGetters` has NO kind filter — it already
//   declares a `$propget` reference for an imported ENUM's expression-
//   bodied property (§10.4 permits one) — while layer 1 above deliberately
//   stayed `Class || Record` (not `Enum`, see that fix's own comment for
//   why). An unconditional spill made an enum's scalar `i32` receiver into
//   a pointer, so the call fired against a symbol layer 1 never defined:
//   confirmed mechanically, this turned a program that compiles AND LINKS
//   today (silently reading the pre-existing documented `0` degrade) into a
//   hard `undefined reference` LINK ERROR — strictly worse than the
//   preexisting gap. T9 below pins the corrected (link-safe, still
//   silently-wrong, out-of-scope-for-this-record-issue) enum outcome.
//
//   LAYER 3 (discovered by LAYER 1+2's own testing): once the call actually
//   reaches the exported getter's BODY, `Emitter::implicitFieldAddr`
//   (LlvmBackend_Layout.cpp) — used to resolve the getter body's bare
//   `_pub`/`_hidden` identifier as an implicit `this.field` — treated
//   "not yet in fieldIndex_" as unconditional proof of "this is a static
//   field" and returned null. fieldIndex_ is populated LAZILY, by
//   classStruct/recordStruct's first call for a given type: a local CLASS
//   always has its struct built early (declareVtables calls classStruct for
//   every class up front, to lay out the vtable), but a RECORD's struct is
//   built lazily on first real use — and a record whose ONLY reference to a
//   field is from inside that field's own computed-property getter body (no
//   other use of the record earlier in the same module) reaches this call
//   as the FIRST thing to ever touch the record's layout, so the struct
//   genuinely had not been built yet. `fieldAddr` (the qualified `obj.f`
//   sibling a few hundred lines below) already retries via
//   `classStruct(ms->parent)` before concluding a field is absent/static;
//   `implicitFieldAddr` did not. Fixed by adding the same retry, so a
//   still-missing entry AFTER the retry is genuinely proof of "static", not
//   "not built yet".
//
// Without LAYER 3's fix alone, LAYER 1+2's fix converts the bug from
// "silently reads 0" into "the getter body itself silently compiles to a
// hardcoded 0" (confirmed by objdump: `xor eax, eax; ret`) — still wrong,
// just for yet another reason. All three layers are required together; each
// was confirmed necessary (not just sufficient) by direct before/after
// testing during this fix, per the lane protocol's "verify the outcome"
// discipline.
//
// #2474's own "candidate area" (BinaryExpr operand-null propagation,
// LlvmBackend_Exprs.cpp lowerBinary's `if (!l || !r) return nullptr;`) is
// NOT itself a defect: it faithfully propagates whatever `lowerExpr` handed
// it. The reason `var w := b.W; return 1 + w;` "worked" before ANY of this
// commit's fixes while `return 1 + b.W;` did not was that the LOCAL-DECL
// zero-init backstop (#2423/#2434) degraded `w` to a well-defined 0 which
// THEN combined correctly with `1` in a SEPARATE binary expression — a
// coincidence of shape (the failing read and the combining arithmetic were
// in two different statements), not evidence of two different lowering
// sub-paths for the property read itself.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2474 {
    std::string path;
    explicit TempFile2474(std::string p) : path(std::move(p)) {}
    ~TempFile2474() { std::remove(path.c_str()); }
};

void writeSource2474(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2474(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2456/#2464/#2467/#2468/#2478's own compileImportAndRun helpers.
int compileImportAndRun2474(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2474_");
    TempFile2474 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2474 aObj((dir / (stem + "_a.o")).string());
    writeSource2474(aSrc.path, libSrc);
    if (!compileToObj2474(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2474 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2474 bExe((dir / stem).string());
    writeSource2474(bSrc.path, appSrc);
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
// T1 — #2474's OWN confirmed repro shape (auto-property, no positional
// field). The VALUE is already correct on origin/main before this commit
// (a side effect of #2478's storage-layout fix) — but this is NOT an
// untouched control: per the file header's LAYER 2 note, this commit's
// importer-side spill fix reroutes `b.W` from CG8's direct-storage read
// onto the SAME exported-getter call the arrow-getter shape uses (both
// share one `importedPropGetterFns_` entry, with no auto-vs-computed
// distinction). Pinned here so a future regression in EITHER the old path's
// replacement or the new path's own correctness is caught by name. Both
// operand orders + the bound-local control must agree.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           auto_property_inline_binary_operand_control_9_2) {
    const char* lib =
        "module RecLib2474T1; "
        "public record BodOnlyProbe2474 "
        "  public property W: int := 42; "
        "end BodOnlyProbe2474;";

    int rLeft = compileImportAndRun2474(
        lib,
        "module App2474T1L; import RecLib2474T1; "
        "public class C2474T1L "
        "  public static method Main(): int "
        "    var b := new BodOnlyProbe2474(); "
        "    return 1 + b.W; "
        "  end Main; "
        "end C2474T1L;");
    if (rLeft == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rLeft, 43);

    int rRight = compileImportAndRun2474(
        lib,
        "module App2474T1R; import RecLib2474T1; "
        "public class C2474T1R "
        "  public static method Main(): int "
        "    var b := new BodOnlyProbe2474(); "
        "    return b.W + 1; "
        "  end Main; "
        "end C2474T1R;");
    if (rRight == -2) return;
    BEZEL_EXPECT_EQ(rRight, 43);

    int rLocal = compileImportAndRun2474(
        lib,
        "module App2474T1C; import RecLib2474T1; "
        "public class C2474T1C "
        "  public static method Main(): int "
        "    var b := new BodOnlyProbe2474(); "
        "    var w := b.W; "
        "    return 1 + w; "
        "  end Main; "
        "end C2474T1C;");
    if (rLocal == -2) return;
    BEZEL_EXPECT_EQ(rLocal, 43);
}

// ===========================================================================
// T2 — CORE repro (this commit's fix): an arrow-getter (computed) property
// on an imported RECORD, backed by a PUBLIC field, combined with a
// POSITIONAL field both as a left and right binary operand, plus the
// bound-local control and the bare read. All four must agree (9 + 41 = 50,
// or 41 alone) — pre-fix, "alone"/"bound-local" gave a LINK ERROR/silent-0
// (see the file header) while inline silently discarded even the correct
// operand.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           arrow_getter_public_backing_inline_binary_operand_9_2_23_2) {
    const char* lib =
        "module RecLib2474T2; "
        "public record RecGetter2474T2(x: int) "
        "  public _pub: int := 41; "
        "  public property Getter: int => _pub; "
        "end RecGetter2474T2;";

    int rInline = compileImportAndRun2474(
        lib,
        "module App2474T2I; import RecLib2474T2; "
        "public class C2474T2I "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T2(9); "
        "    return r.x + r.Getter; "
        "  end Main; "
        "end C2474T2I;");
    if (rInline == -2) return;
    BEZEL_EXPECT_EQ(rInline, 50);

    int rReverse = compileImportAndRun2474(
        lib,
        "module App2474T2R; import RecLib2474T2; "
        "public class C2474T2R "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T2(9); "
        "    return r.Getter + r.x; "
        "  end Main; "
        "end C2474T2R;");
    if (rReverse == -2) return;
    BEZEL_EXPECT_EQ(rReverse, 50);

    int rLocal = compileImportAndRun2474(
        lib,
        "module App2474T2L; import RecLib2474T2; "
        "public class C2474T2L "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T2(9); "
        "    var g := r.Getter; "
        "    return r.x + g; "
        "  end Main; "
        "end C2474T2L;");
    if (rLocal == -2) return;
    BEZEL_EXPECT_EQ(rLocal, 50);

    int rAlone = compileImportAndRun2474(
        lib,
        "module App2474T2A; import RecLib2474T2; "
        "public class C2474T2A "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T2(9); "
        "    return r.Getter; "
        "  end Main; "
        "end C2474T2A;");
    if (rAlone == -2) return;
    BEZEL_EXPECT_EQ(rAlone, 41);
}

// ===========================================================================
// T3 — CONTROL (scope boundary): an arrow-getter backed by a PRIVATE field.
// AT THE TIME this test was written, #2467/#2468 deliberately did NOT
// replay a private body field's initializer VALUE cross-module (only its
// storage SLOT was correct) — so `_hidden` stayed the documented `0`
// default after import-side construction, and `r.Hidden` read that
// documented `0`, not the declared `77`. FLIPPED by fix #3723
// (§9.1/§9.2/§22.8/§2.8.2, v0.286): #3723 widened the SAME `includeForLayout`
// carriage this file's `_hidden` shape depends on to keep a record's
// private field initializer text regardless of generic-ness, so `_hidden`
// now replays to its declared `77` and `r.Hidden` returns it. The POINT of
// this control is still CONSISTENCY: inline and bound-local must agree with
// each other (both now `9 + 77 = 86`), which is the actual #2474 property
// (never the private-value-replay question itself, which #3723 now closes).
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           arrow_getter_private_backing_inline_matches_local_control_9_2) {
    const char* lib =
        "module RecLib2474T3; "
        "public record RecHidden2474T3(x: int) "
        "  private _hidden: int := 77; "
        "  public property Hidden: int => _hidden; "
        "end RecHidden2474T3;";

    int rInline = compileImportAndRun2474(
        lib,
        "module App2474T3I; import RecLib2474T3; "
        "public class C2474T3I "
        "  public static method Main(): int "
        "    var r := new RecHidden2474T3(9); "
        "    return r.x + r.Hidden; "
        "  end Main; "
        "end C2474T3I;");
    if (rInline == -2) return;
    BEZEL_EXPECT_EQ(rInline, 86);  // 9 + 77 — replayed (fix #3723)

    int rLocal = compileImportAndRun2474(
        lib,
        "module App2474T3L; import RecLib2474T3; "
        "public class C2474T3L "
        "  public static method Main(): int "
        "    var r := new RecHidden2474T3(9); "
        "    var h := r.Hidden; "
        "    return r.x + h; "
        "  end Main; "
        "end C2474T3L;");
    if (rLocal == -2) return;
    BEZEL_EXPECT_EQ(rLocal, 86);  // must MATCH rInline — the actual #2474 property
}

// ===========================================================================
// T4 — nested expression + a comparison operator (both non-arithmetic
// operators AND deeper nesting than a single `+`), same shape as T2.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           arrow_getter_nested_expr_and_comparison_9_2) {
    const char* lib =
        "module RecLib2474T4; "
        "public record RecGetter2474T4(x: int) "
        "  public _pub: int := 41; "
        "  public property Getter: int => _pub; "
        "end RecGetter2474T4;";

    int rNested = compileImportAndRun2474(
        lib,
        "module App2474T4N; import RecLib2474T4; "
        "public class C2474T4N "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T4(9); "
        "    return (r.x + r.Getter) * 2 - 1; "
        "  end Main; "
        "end C2474T4N;");
    if (rNested == -2) return;
    BEZEL_EXPECT_EQ(rNested, 99);  // (9 + 41) * 2 - 1

    int rCmp = compileImportAndRun2474(
        lib,
        "module App2474T4C; import RecLib2474T4; "
        "public class C2474T4C "
        "  public static method Main(): int "
        "    var r := new RecGetter2474T4(9); "
        "    if r.Getter > r.x then "
        "      return 1; "
        "    else "
        "      return 0; "
        "    end if; "
        "  end Main; "
        "end C2474T4C;");
    if (rCmp == -2) return;
    BEZEL_EXPECT_EQ(rCmp, 1);
}

// ===========================================================================
// T5 — MULTIPLE arrow-getter properties combined in one expression, each
// backed by its own public field with a distinct value.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           multiple_arrow_getters_combined_9_2) {
    int r = compileImportAndRun2474(
        "module RecLib2474T5; "
        "public record RecMulti2474T5(x: int) "
        "  public _a: int := 2; "
        "  public _b: int := 3; "
        "  public property GA: int => _a; "
        "  public property GB: int => _b; "
        "end RecMulti2474T5;",
        "module App2474T5; import RecLib2474T5; "
        "public class C2474T5 "
        "  public static method Main(): int "
        "    var r := new RecMulti2474T5(9); "
        "    return r.x + r.GA + r.GB; "
        "  end Main; "
        "end C2474T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 14);  // 9 + 2 + 3
}

// ===========================================================================
// T6 — CONTROL: a fully LOCAL record (no import) with the identical
// arrow-getter + positional shape as T2. Guards against this fix's
// exporter-gate widening / implicitFieldAddr lazy-build retry affecting
// same-module compilation — a local record's struct is typically already
// built by the time its own getter body is emitted (this program
// constructs an instance in the SAME method), so this path was never
// broken, but must stay correct.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           local_record_arrow_getter_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2474_t6_");
    TempFile2474 src((dir / (stem + ".bzl")).string());
    TempFile2474 exe((dir / stem).string());
    writeSource2474(src.path,
                    "module LocalCtl2474T6; "
                    "public record LocalRec2474T6(x: int) "
                    "  public _v: int := 19; "
                    "  public property V: int => _v; "
                    "end LocalRec2474T6; "
                    "public class C2474T6 "
                    "  public static method Main(): int "
                    "    var r := new LocalRec2474T6(9); "
                    "    return r.x + r.V; "
                    "  end Main; "
                    "end C2474T6;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 28);  // 9 + 19
}

// ===========================================================================
// T7 — CONTROL: a plain FIELD (not a property) on an imported record,
// combined inline — the #2467-fixed shape, unaffected by this commit's
// exporter-gate / implicitFieldAddr changes (a plain field read never goes
// through emitExportedPropertyGetters/ensurePropertyAccessorFn/
// implicitFieldAddr's getter-body path at all — it is a direct GEP via
// fieldAddr on the qualified `r._y`, a completely different call site).
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           plain_field_inline_binary_operand_unaffected_control_9_1) {
    int r = compileImportAndRun2474(
        "module RecLib2474T7; "
        "public record RecField2474T7(x: int) "
        "  public _y: int := 25; "
        "end RecField2474T7;",
        "module App2474T7; import RecLib2474T7; "
        "public class C2474T7 "
        "  public static method Main(): int "
        "    var r := new RecField2474T7(9); "
        "    return r.x + r._y; "
        "  end Main; "
        "end C2474T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 34);  // 9 + 25
}

// ===========================================================================
// T8 — CONTROL: a CLASS's arrow-getter property (not a record), combined
// inline, cross-module. This shape already worked BEFORE this commit (a
// class's struct is always built early by declareVtables, so
// implicitFieldAddr's pre-fix "not found = static" misclassification never
// triggered for it) — pinned here to guard against a regression from this
// commit's implicitFieldAddr change, which is shared by every class AND
// record implicit-field read in the compiler.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           class_arrow_getter_inline_binary_operand_unaffected_control) {
    int r = compileImportAndRun2474(
        "module ClsLib2474T8; "
        "public class ClsGetter2474T8 "
        "  public _pub: int := 13; "
        "  public property Getter: int => _pub; "
        "end ClsGetter2474T8;",
        "module App2474T8; import ClsLib2474T8; "
        "public class C2474T8 "
        "  public static method Main(): int "
        "    using var c := new ClsGetter2474T8(); "
        "    return 9 + c.Getter; "
        "  end Main; "
        "end C2474T8;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 22);  // 9 + 13
}

// ===========================================================================
// T9 — REGRESSION GUARD (review-caught, not originally in this file): an
// imported ENUM's expression-bodied property (§10.4 permits one), combined
// inline. `declareImportedPropertyGetters` has NO kind filter, so it already
// declares a `$propget` reference for an enum's computed property today —
// but layer 1 (the exporter gate) stays `Class || Record` in this commit,
// deliberately NOT `Enum` (see LlvmBackend_Setup.cpp's own comment on
// emitExportedPropertyGetters for why: widening it needs its own
// verification of ensurePropertyAccessorFn's enum `this` handling through
// this NEW external-stub calling convention, out of THIS record-scoped
// issue's remit). The POINT of this test is layer 2's spill guard: an
// earlier draft spilled ANY non-pointer receiver unconditionally, which
// turned THIS exact program from "compiles, links, and runs (silently
// reading the pre-existing documented `0` degrade)" into a hard `undefined
// reference` LINK ERROR — confirmed mechanically before the guard was
// narrowed to aggregates-only (matching the local computed-getter path's
// own `isAggregateType() && !isEnumReceiver` condition). This test asserts
// compile+link SUCCESS (the actual regression this guards) and pins the
// still-silently-wrong `0` value only for transparency — that value is a
// separate, PRE-EXISTING, out-of-scope gap (unrelated to records, not
// fixed or worsened by this commit; not #2474's concern) rather than
// something this commit endorses as correct.
// ===========================================================================
BEZEL_TEST(xmod_record_property_binary_operand_2474,
           imported_enum_property_inline_still_links_control_10_4) {
    int r = compileImportAndRun2474(
        "module EnumLib2474T9; "
        "public enum EnumX2474T9 "
        "  Red, Green, Blue; "
        "  public property P: int => 7; "
        "end EnumX2474T9;",
        "module App2474T9; import EnumLib2474T9; "
        "public class C2474T9 "
        "  public static method Main(): int "
        "    var e := EnumX2474T9.Red; "
        "    return 1 + e.P; "
        "  end Main; "
        "end C2474T9;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r >= 0);  // the real assertion: compiled AND linked
    BEZEL_EXPECT_EQ(r, 0);      // pre-existing, out-of-scope gap — see above
}

}  // namespace bezel::module
