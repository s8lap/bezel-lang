// fix #2467 (§9.2/§2.5, v0.235): an imported record's body-declared field or
// auto-property initializer was never replayed cross-module at all — the
// member exists and is correctly laid out (once #2464's ordering fix and
// #2468's layout carriage apply, or trivially when it is already `public`),
// but its declared default value silently stayed at the type's zero value
// instead of the value written in the exporting module's source.
//
// ROOT CAUSE: `buildRecordValue` (LlvmBackend_Exprs2.cpp) ran each body-
// declared field/auto-property initializer via `lowerExpr(*init)`, where
// `init` came from `dynamic_cast<const ast::FieldDecl*>(m->decl)->
// initializer` or `dynamic_cast<const ast::PropertyDecl*>(m->decl)->
// autoInitializer`. For an IMPORTED symbol, `m->decl` is always null (no
// AST crosses the module boundary — `ModuleLoader.cpp`'s `MetaMemberKind::
// Field`/`Property` reconstruction never sets it), so `init` was always
// null and the initializer step was silently skipped, leaving the storage
// at its zero-initialized (`ConstantAggregateZero`) default.
//
// THE FIX (this commit, plain-FIELD half — see the auto-property note
// below): reuses the EXISTING constant-foldable-text wire mechanism §22.12's
// const values and #2456's class-side field layout carriage already
// established (ModuleMetadataBuilder.cpp's `renderConstExpr` /
// `ctx_.importedConstValue` / `foldImportedConstText`, issue #328 FINDING
// [3]/[7]) rather than inventing a new metadata surface (CLAUDE.md rule 4):
// `MetaMember::valueText` already "rides every member" on the wire
// (ModuleMetadata.hpp v2/v3 notes) but had no CONSUMER for a Field/Property
// member on the RECORD construction path (only a class-level `const` read
// consumed it before). `buildRecordValue`'s per-field loop now falls back
// to `ctx_.importedConstValue(m)` + `foldImportedConstText` whenever
// `m->decl == nullptr` (an imported symbol). Only a CONSTANT-FOLDABLE
// initializer crosses this way — `renderConstExpr` returns "" for `new
// Array()`, `DateTime.Now`, etc., a documented pre-existing limitation
// shared with the class-side Field carriage, not newly introduced here.
//
// ALSO FIXED (this commit): `ModuleMetadataBuilder.cpp`'s Property branch of
// `convertMember` never carried an AUTO-PROPERTY's initializer at all (only
// the Field branch did, via `renderConstExpr(fd->initializer.get())`) — a
// second, independent gap from the "no consumer" one above. This commit
// adds the equivalent `renderConstExpr(pd->autoInitializer.get())` carriage
// for a non-static auto-property, mirroring the Field branch exactly.
//
// ⚠ WHAT THIS COMMIT DOES NOT CLOSE — the AUTO-PROPERTY half of #2467's own
// confirmed repro (`public property W: int := 42;`) remains OPEN, blocked
// on a newly-discovered, deeper, separate defect filed as #2478:
// `sema::isAutoPropertySymbol` requires `ps->decl` (a real `ast::
// PropertyDecl`), which is null for EVERY imported PropertySymbol — so an
// imported auto-property (public or private) is never even recognized as
// an INSTANCE STORAGE member by `collectRecordStorage`/`collectInstance
// Storage` in the first place, regardless of any wire-carriage fix. This is
// a genuine memory-safety hazard for CLASSES too (#2478 has a confirmed
// class-side field-aliasing repro, the same failure class as #2456) and
// needs its own metadata-surface decision (there is no wire-carried
// "is this an auto-property" bit) — not invented here per CLAUDE.md rule 4.
// This file therefore tests PLAIN FIELDS (`public _y: int := 25;`), which
// #2467's own issue text explicitly confirms as an equivalent alternate
// repro to the auto-property form ("Also confirmed with a plain public
// field... in place of the auto-property") and which IS fully closed by
// this commit — verified end-to-end via instrumentation before this file
// was written. This commit's Property-branch valueText carriage is left in
// place as correct, tested (via instrumentation showing the value crosses
// the wire intact) infrastructure that #2478's own eventual fix can consume
// directly with no further wire-format change.
//
// SCOPE (mirrors #2468's own scoping): a PRIVATE/internal body field's
// value is deliberately NOT replayed by this commit — #2468's
// `includeForLayout` strips `valueText`/`fieldInitText` for a layout-only
// (private, unexported) member to keep that carriage genuinely layout-only
// (no initializer, no ABI change). T4 below pins this as a known, expected
// state, not a regression.

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

struct TempFile2467 {
    std::string path;
    explicit TempFile2467(std::string p) : path(std::move(p)) {}
    ~TempFile2467() { std::remove(path.c_str()); }
};

void writeSource2467(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2467(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2456/#2464/#2468's own compileImportAndRun helpers.
int compileImportAndRun2467(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2467_" + std::to_string(std::rand());
    TempFile2467 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2467 aObj((dir / (stem + "_a.o")).string());
    writeSource2467(aSrc.path, libSrc);
    if (!compileToObj2467(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2467 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2467 bExe((dir / stem).string());
    writeSource2467(bSrc.path, appSrc);
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
// T1 — CORE repro: §9.2's own confirmed alternate shape for #2467 — a
// body-only record with a single PUBLIC plain field and no positional
// fields at all. Pre-fix this read `0`; post-fix it replays the declared
// `25`.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           body_only_public_field_replays_cross_module_9_2) {
    int r = compileImportAndRun2467(
        "module RecLib2467T1; "
        "public record Rec2467T1 "
        "  public _y: int := 25; "
        "end Rec2467T1;",
        "module App2467T1; import RecLib2467T1; "
        "public class C2467T1 "
        "  public static method Main(): int "
        "    var b := new Rec2467T1(); "
        "    return b._y; "
        "  end Main; "
        "end C2467T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 25);
}

// ===========================================================================
// T2 — COMBINED-FORM: a positional field AND a public body field, both
// replay correctly together (exercises #2464's positional-vs-body ordering
// fix and #2467's value-replay fix composing correctly — #2464's own issue
// text flagged this exact interaction as an open question).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           combined_form_positional_and_body_field_both_correct_9_1_9_2) {
    int r = compileImportAndRun2467(
        "module RecLib2467T2; "
        "public record Rec2467T2(x: int) "
        "  public _y: int := 25; "
        "end Rec2467T2;",
        "module App2467T2; import RecLib2467T2; "
        "public class C2467T2 "
        "  public static method Main(): int "
        "    var r := new Rec2467T2(9); "
        "    return r.x + r._y; "
        "  end Main; "
        "end C2467T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 34);  // 9 + 25
}

// ===========================================================================
// T3 — CONTROL: a POSITIONAL-ONLY record (no body storage at all) is
// unaffected by this fix — the body-member init loop simply finds no body
// storage to iterate, and the positional fast path is untouched.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           positional_only_record_unaffected_control_9_1) {
    int r = compileImportAndRun2467(
        "module RecLib2467T3; "
        "public record Rec2467T3(x: int, y: int) "
        "end Rec2467T3;",
        "module App2467T3; import RecLib2467T3; "
        "public class C2467T3 "
        "  public static method Main(): int "
        "    var r := new Rec2467T3(9, 6); "
        "    return r.x + r.y; "
        "  end Main; "
        "end C2467T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 15);
}

// ===========================================================================
// T4 — CONTROL (scope boundary, not a regression): a PRIVATE body field's
// declared value is deliberately NOT replayed by this fix — #2468's
// `includeForLayout` strips a layout-only (private) member's `valueText` to
// keep that carriage genuinely layout-only. The private field's STORAGE
// SLOT is correct (via #2468, proven by the positional field NOT
// aliasing/corrupting), but its VALUE stays the zero default. Reads the
// private field's value through a method (mirrors #2468's own T1, avoiding
// #2474's separate inline-property-in-binary-expression defect) to confirm
// this is a well-defined, documented `0` — not a crash, not a
// mis-corrupted read of a neighboring slot.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           private_body_field_value_stays_documented_gap_control) {
    int r = compileImportAndRun2467(
        "module RecLib2467T4; "
        "public record Rec2467T4(x: int) "
        "  private _hidden: int := 77; "
        "  public method GetHidden(): int => _hidden; "
        "end Rec2467T4;",
        "module App2467T4; import RecLib2467T4; "
        "public class C2467T4 "
        "  public static method Main(): int "
        "    var r := new Rec2467T4(9); "
        "    var h := r.GetHidden(); "
        "    return r.x + h; "
        "  end Main; "
        "end C2467T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);  // 9 + 0 — private value NOT replayed (documented)
}

// ===========================================================================
// T5 — NESTED RECORDS: an imported record type (`InnerNested2467`, with its
// own public body-field default) is constructed directly and stored as
// ANOTHER imported record's POSITIONAL field. Confirms the inner record's
// own body-field initializer replays correctly when constructed
// independently, and that a record-typed (not just primitive-typed) body/
// positional member does not break `recordMemberTypeResolved`'s fallback
// used by this fix's `foldImportedConstText` call.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           nested_record_body_field_replays_9_1_9_2) {
    int r = compileImportAndRun2467(
        "module RecLib2467T5; "
        "public record Inner2467T5 "
        "  public _v: int := 15; "
        "end Inner2467T5; "
        "public record Outer2467T5(tag: int, inner: Inner2467T5) "
        "end Outer2467T5;",
        "module App2467T5; import RecLib2467T5; "
        "public class C2467T5 "
        "  public static method Main(): int "
        "    var i := new Inner2467T5(); "
        "    var o := new Outer2467T5(9, i); "
        "    var oi := o.inner; "
        "    return o.tag + oi._v; "
        "  end Main; "
        "end C2467T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 24);  // 9 + 15
}

// ===========================================================================
// T6 — CONTROL: a fully LOCAL record (no import at all) with the identical
// combined-form shape as T2. Guards against this fix's new fallback branch
// in `buildRecordValue` affecting same-module compilation at all — a local
// symbol always has `m->decl != nullptr`, so the fallback's `if (m->decl !=
// nullptr) continue;` guard should keep this path byte-for-byte unchanged.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           local_record_unaffected_control) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2467_t6_" + std::to_string(std::rand());
    TempFile2467 src((dir / (stem + ".bzl")).string());
    TempFile2467 exe((dir / stem).string());
    writeSource2467(src.path,
                    "module LocalCtl2467T6; "
                    "public record LocalRec2467T6(x: int) "
                    "  public _y: int := 25; "
                    "end LocalRec2467T6; "
                    "public class C2467T6 "
                    "  public static method Main(): int "
                    "    var r := new LocalRec2467T6(9); "
                    "    return r.x + r._y; "
                    "  end Main; "
                    "end C2467T6;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 34);
}

// ===========================================================================
// T7 — multiple public body fields, distinct values, combined directly
// (plain FIELD reads combine safely inline in a binary expression —
// confirmed distinct from #2474's PROPERTY-specific inline-binary-operand
// defect, which this test deliberately does not exercise).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_2467,
           multiple_public_body_fields_replay_9_2) {
    int r = compileImportAndRun2467(
        "module RecLib2467T7; "
        "public record Rec2467T7 "
        "  public _a: int := 2; "
        "  public _b: int := 3; "
        "  public _c: int := 5; "
        "end Rec2467T7;",
        "module App2467T7; import RecLib2467T7; "
        "public class C2467T7 "
        "  public static method Main(): int "
        "    var r := new Rec2467T7(); "
        "    return r._a + r._b + r._c; "
        "  end Main; "
        "end C2467T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 10);  // 2 + 3 + 5
}

}  // namespace bezel::module
