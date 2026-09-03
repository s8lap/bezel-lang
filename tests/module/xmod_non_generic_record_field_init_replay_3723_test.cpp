// fix #3723 (§9.1 / §9.2 / §22.8 / §2.8.2, v0.286): a NON-generic imported
// record's non-constant field initializer was never carried across the
// module boundary AT ALL — the writer-side half of the #3671 duplication.
//
// #3671 (d6ce155 / 79146cd) fixed `buildRecordValue`'s (LlvmBackend_Exprs2.cpp)
// CONSUMER side: it now tries the full parsed-AST-expr replay
// (`ctx_.importedInstanceFieldInitExpr`) before falling back to the
// constant-fold-only path, exactly like `emitOwnFieldInit` already does for a
// class. But that consumer-side fix only reaches a GENERIC imported record,
// because `ModuleMetadataBuilder.cpp`'s `convertMember` FieldDecl branch
// gated `out.fieldInitText` on `isGenericType` alone — the identical
// asymmetry fix #2525 already closed for the ConstructorSymbol branch's
// `ctorBodyText` (`(isGenericType || Record)`, because a record has "no
// cross-module call of any kind to replay a field initializer through,
// generic or not" — §9.1/§9.2 record construction is lowered INLINE BY
// VALUE at the `new` site).
//
// THE FIX (this commit, src/module/ModuleMetadataBuilder.cpp):
//   1. The FieldDecl branch's gate (~line 1075) widens from `isGenericType`
//      to `(isGenericType || isRecordType)`, where `isRecordType` reads
//      `m.parent->kind == ast::TypeDeclKind::Record` — the SAME predicate
//      #2525 uses on `ctor->parent->kind`, applied here via `MemberSymbol`'s
//      own `parent` pointer (every MemberSymbol carries one, not just
//      ConstructorSymbol).
//   2. A SECOND, independently-drifting copy of the identical
//      `isGenericMonomorphized`-alone question lives ~20 lines below, in the
//      `includeForLayout` (private/internal field) carriage: it cleared
//      `fieldInitText` for `!isGenericMonomorphized`, silently reproducing
//      the SAME bug for a record's PRIVATE fields even after fix (1) above.
//      Widened identically: keep the text for a Record regardless of
//      generic-ness. T6 below is the regression pin for this second gate —
//      LANE_RULES §6b requires the family land WHOLE, not half.
//
// A non-generic CLASS is deliberately EXCLUDED from both widenings: a class
// (generic or not) replays every instance field initializer through its own
// exported `bz$<Type>$init` (#2452) — for a NON-generic class that function
// is a real cross-module CALL, so no textual carriage is needed at all (only
// a GENERIC class needs it, because the importer monomorphizes construction
// itself with no compiled function to call — the existing `isGenericType`
// half of the gate already covers that). T5 below pins that a non-generic
// class's owned/call-initialized fields were ALREADY correct before this
// fix and remain so — this lane does not touch that path.
//
// The reader side (ModuleLoader.cpp's `parseAndAttachFieldInit` driving
// loop, ~line 4148-4202) is unconditional on `mm.fieldInitText.empty()` with
// NO TypeDeclKind or generic check at all — already admits any Record,
// confirmed by reading the loop; no reader change needed. `buildRecordValue`
// (LlvmBackend_Exprs2.cpp, fixed by #3671/79146cd) is likewise a pure
// per-member side-table read with no generic gate of its own — once the
// wire carries the data, the already-fixed consumer picks it up with zero
// further changes, exactly as #3671's own commit body predicted.
//
// No schema bump: `fieldInitText` already exists on the wire (added by
// #2860) and this only widens WHICH members populate an already-serialized-
// but-previously-often-empty field — the identical precedent #2525's
// ctorBodyText widening set (that commit touched ModuleMetadataBuilder.cpp/
// ModuleLoader.cpp only, no ModuleMetadata.hpp / kSchemaVersion change).
// Per #3648's discussion, a schema bump is required only when an EXISTING,
// already-populated wire value's MEANING changes for previously-non-empty
// data (a verdict flip) — here the field was empty for this shape before and
// is populated now; no consumer's interpretation of already-carried data
// changes.
//
// §6b exclusion diff — what the narrower `isGenericType`-alone gate
// excluded, and what this fix now admits, verified on a real two-module
// build:
//   - non-generic record, PUBLIC field, call init        -- was 0, now replays (T1)
//   - non-generic record, PUBLIC field, `new` init        -- was 0, now replays (T2)
//   - non-generic record, PUBLIC field, member-read init  -- was 0, now replays (T3)
//   - non-generic record, PUBLIC field, conditional init  -- was 0, now replays (T4)
//   - non-generic CLASS, owned/call-initialized fields    -- ALREADY correct via
//     `bz$<Type>$init`; UNCHANGED by this fix (T5, T9)
//   - non-generic record, PRIVATE/layout-only field, call init -- was 0 (aliased/
//     zeroed at the `includeForLayout` clearing gate), now replays (T6)
//   - non-generic record, field referencing an INTERNAL sibling (uncarriable
//     even after the widening — the reconstruction genuinely cannot resolve
//     it) -- was silently 0 with NO diagnostic; now a loud §2.8.2 reject,
//     naming the field, at the construction site (T7) — the SAME
//     construction-site diagnostic #3671/79146cd added for the generic case,
//     now reachable for the non-generic shape too because
//     `importedInstanceFieldInitText` is populated for it
//   - STATIC fields of a non-generic RECORD     -- UNCHANGED end-to-end
//     behavior (external storage global, §22.12 rule 1; T10/T11), but this
//     required an EXPLICIT `!isStaticField` exclusion added to the newly-
//     widened `isRecordType` arm — a review follow-up caught that the
//     FIRST version of this fix had NO such exclusion, so a static field
//     with an uncarriable (internal-sibling) initializer silently started
//     taking the generic-only text-carriage/reject path and HARD-REJECTED
//     an import that the parent commit correctly compiled and ran (T10 is
//     the regression pin). A GENERIC record's static field is the OPPOSITE
//     case and must KEEP carrying the text regardless of `isStaticField`
//     (#2860/#2871: each instantiation needs its own per-consumer replay,
//     T12 control, unaffected by this exclusion).
//   - STATIC fields of a non-generic CLASS       -- UNCHANGED (external
//     storage global; classes were never touched by either widening)
//   - CONST fields (already constant-folded via `valueText`)  -- UNCHANGED
//     (this fix touches only `fieldInitText`, never `valueText`)
//   - a fully LOCAL (non-imported) non-generic record       -- UNCHANGED (T8)
//   - interpolated (`$"..."`) initializers -- STILL WRONG on this fix as on
//     #3671's generic case (pre-existing #3724 defect in the shared #2874
//     load-time reconstruction, independent of class/record/generic-ness);
//     this lane does not widen into that shape (no test here exercises it,
//     per the brief's explicit instruction).
//
// FAIL-WITHOUT / PASS-WITH: reverting `src/module/ModuleMetadataBuilder.cpp`
// to its parent commit (in a separate scratch copy, per LANE_RULES
// R12-H) and relinking `bezelc` makes T1-T4, T6 and T7 fail (T1-T4/T6 read
// back the §7.9 zero default instead of the exporter's declared value; T7's
// diagnostic never fires because `importedInstanceFieldInitText` is never
// populated for the non-generic shape, so `compileImportCaptureErr3723`
// reports success instead of a reject). T10's FAIL-WITHOUT is against the
// INTERMEDIATE (review-flagged, `!isStaticField`-less) version of this
// commit's own gate, not the parent: that version makes T10 fail-to-compile
// (the §2.8.2 reject) where the parent AND the final version both compile
// and run it correctly — see this commit's REVIEW FINDINGS ADDRESSED note.
// T5, T8, T9, T11, T12 pass unconditionally on every tree (unaffected
// controls).

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

struct TempFile3723 {
    std::string path;
    explicit TempFile3723(std::string p) : path(std::move(p)) {}
    ~TempFile3723() { std::remove(path.c_str()); }
};

void writeSource3723(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool buildLib3723(const std::string& stem, const std::string& libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource3723(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src, objOut).success;
    std::remove(src.c_str());
    return ok && std::ifstream(objOut).good();
}

// Compile B (importing A) -> exe, run, report exit (-1 on any compile/link/
// run failure, matching the sibling #2467/#2874/#3671 test files' convention).
int compileImportAndRun3723(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Same shape as #2874/#3671's diagnostic helper: compile B importing A,
// capturing stderr, so a test can assert the diagnostic TEXT and LOCATION
// rather than just "it failed".
bool compileImportCaptureErr3723(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() / ("bz3723_err_" + unique_stem("") + ".txt"))
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
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
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

// The library shared by T1-T5: a NON-GENERIC record whose body fields cover
// the four shapes the narrow gate zeroed and now replays (call/new/member
// read/conditional), plus a NON-generic CLASS twin (T5's control) declaring
// the identical shapes to confirm the class path was already correct and is
// unaffected by this fix.
const char* kLib3723 =
    "module Lib3723; "
    "public class H3723 "
    "  public static Val: int := 11; "
    "  public static method Get(): int => 42; "
    "end H3723; "
    "public record Pt3723(x: int, y: int) "
    "end Pt3723; "
    "public record R3723(a: int) "
    "  public bCall: int := H3723.Get(); "
    "  public bNew: Pt3723 := new Pt3723(3, 4); "
    "  public bMember: int := H3723.Val; "
    "  public bCond: int := (if H3723.Get() > 0 then 1 else 2); "
    "end R3723; "
    "public class C3723Ctl "
    "  public v: int := 5; "
    "  public bCall: int := H3723.Get(); "
    "  public bNew: Pt3723 := new Pt3723(3, 4); "
    "  public bMember: int := H3723.Val; "
    "  public bCond: int := (if H3723.Get() > 0 then 1 else 2); "
    "end C3723Ctl;";

std::string appSrc3723(const std::string& moduleName, const std::string& lib,
                       const std::string& body) {
    return "module " + moduleName + ";\n"
           "import " + lib + ";\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// T1 — CALL shape: `public bCall: int := H3723.Get();` on a NON-generic
// record. Pre-fix this read `0` (the wire never carried the data at all for
// this shape — #3671's own confirmed repro). Post-fix it replays the
// exporter's declared `42`.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t1_non_generic_record_call_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3723_t1_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem, kLib3723, aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T1", "Lib3723",
                   "    var r := new R3723(9);\n"
                   "    return r.bCall;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T2 — `new` shape: `public bNew: Pt3723 := new Pt3723(3, 4);`. Pre-fix
// `bNew` read the zero-valued `Pt3723(0, 0)`. Post-fix it replays the
// exporter's declared `Pt3723(3, 4)`.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t2_non_generic_record_new_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3723_t2_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem, kLib3723, aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T2", "Lib3723",
                   "    var r := new R3723(9);\n"
                   "    return r.bNew.x * 10 + r.bNew.y;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 34);
}

// ===========================================================================
// T3 — static MEMBER READ shape: `public bMember: int := H3723.Val;`.
// Pre-fix `bMember` read `0`. Post-fix it replays the exporter's declared
// `11`.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t3_non_generic_record_member_read_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3723_t3_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem, kLib3723, aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T3", "Lib3723",
                   "    var r := new R3723(9);\n"
                   "    return r.bMember;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 11);
}

// ===========================================================================
// T4 — CONDITIONAL shape: `public bCond: int := (if H3723.Get() > 0 then 1
// else 2);`. Pre-fix `bCond` read `0`. Post-fix it replays the exporter's
// declared `1`.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t4_non_generic_record_conditional_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3723_t4_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem, kLib3723, aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T4", "Lib3723",
                   "    var r := new R3723(9);\n"
                   "    return r.bCond;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 1);
}

// ===========================================================================
// T5 — CONTROL: a NON-generic CLASS with the identical four shapes
// (`C3723Ctl`) was ALREADY correct before this fix (its `bz$<Type>$init`
// replays every field initializer via a real cross-module call, #2452) and
// this fix deliberately does NOT widen the writer's gate for a non-generic
// class (§6b: "inert data with no consumer" — `buildRecordValue`, the sole
// reader of `fieldInitText` for a non-generic type, is record-only). Pins
// that the class path stays correct, unaffected either way.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t5_non_generic_class_control_already_correct_unaffected) {
    std::string stem = unique_stem("bz3723_t5_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem, kLib3723, aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T5", "Lib3723",
                   "    var c := new C3723Ctl();\n"
                   "    var bad := 0;\n"
                   "    if c.bCall <> 42 then bad := bad + 1; end if;\n"
                   "    if c.bNew.x <> 3 then bad := bad + 2; end if;\n"
                   "    if c.bNew.y <> 4 then bad := bad + 4; end if;\n"
                   "    if c.bMember <> 11 then bad := bad + 8; end if;\n"
                   "    if c.bCond <> 1 then bad := bad + 16; end if;\n"
                   "    if c.v <> 5 then bad := bad + 32; end if;\n"
                   "    return bad;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 0);
}

// ===========================================================================
// T6 — PRIVATE/LAYOUT-ONLY FIELD (the SECOND gate this fix widens, §6b):
// a private body field, read back only through a PUBLIC method (mirrors
// #2468's own method-call-marshalling repro, T1). Before this fix, the
// `includeForLayout` clearing gate (~line 2029) cleared `fieldInitText` for
// `!isGenericMonomorphized` unconditionally, silently reproducing #3723's
// bug for a private record field even after the FieldDecl branch's own gate
// (line ~1075) was widened — a second, independently-drifting copy of the
// same predicate mistake. Post-fix, `_hidden`'s call initializer replays
// through `GetHidden()`'s cross-module method call exactly like a public
// field's initializer now does.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t6_private_layout_only_field_replays_via_method_marshalling_9_1_22_8) {
    std::string stem = unique_stem("bz3723_t6_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723Priv; "
        "public class H3723Priv "
        "  public static method Get(): int => 55; "
        "end H3723Priv; "
        "public record Rec3723Priv(x: int) "
        "  private _hidden: int := H3723Priv.Get(); "
        "  public method GetHidden(): int => _hidden; "
        "end Rec3723Priv;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T6", "Lib3723Priv",
                   "    var r := new Rec3723Priv(9);\n"
                   "    var h := r.GetHidden();\n"
                   "    return r.x + h;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 64);
}

// ===========================================================================
// T7 — LOUD REJECT (§2.8.2 / §6c of this run's lane rules), non-generic twin
// of the existing generic T7
// (xmod_record_body_field_init_replay_generic_3671_test.cpp): a NON-generic
// record whose field initializer references an INTERNAL sibling (not part
// of the exported public surface, §13.2). The load-time reconstruction
// cannot resolve it, and `importedInstanceFieldInitText` IS now present
// (this fix's own widening carried the fact "this field has an
// initializer") — so `buildRecordValue`'s #3671/79146cd diagnostic fires,
// converting what was previously a SILENT §7.9-zero (no diagnostic at any
// severity, per #3723's own issue text) into a located reject naming the
// field and type.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t7_uncarriable_non_generic_record_initializer_rejects_loudly_2_8_2) {
    std::string stem = unique_stem("bz3723_t7_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723Reject; "
        "internal class Secret3723 "
        "  internal static method Get(): int => 7; "
        "end Secret3723; "
        "public record R3723Reject(a: int) "
        "  public b: int := Secret3723.Get(); "
        "end R3723Reject;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T7", "Lib3723Reject",
                   "    var r := new R3723Reject(9);\n"
                   "    return r.b;\n"));
    std::string err;
    bool ok = compileImportCaptureErr3723(bSrc.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot run the initializer of its field") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`b`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("R3723Reject") != std::string::npos);
}

// ===========================================================================
// T8 — CONTROL: a fully LOCAL non-generic record (no import at all), same
// shape as T1. Guards against this fix's writer-side widening affecting
// same-module compilation — a local record's field always reads its AST
// initializer directly and never touches `fieldInitText` at all.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t8_local_non_generic_record_unaffected_control) {
    bezel::Driver driver;
    std::string stem = unique_stem("bz3723_t8_");
    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 src((dir / (stem + ".bzl")).string());
    TempFile3723 exe((dir / stem).string());
    writeSource3723(src.path,
                    "module LocalCtl3723; "
                    "public class HL3723 "
                    "  public static method Get(): int => 42; "
                    "end HL3723; "
                    "public record LR3723(a: int) "
                    "  public b: int := HL3723.Get(); "
                    "end LR3723; "
                    "public class C3723T8 "
                    "  public static method Main(): int "
                    "    var r := new LR3723(9); "
                    "    return r.b; "
                    "  end Main; "
                    "end C3723T8;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T9 — CONTROL, UNDER ASAN, 200 ITERATIONS (dispose once per iteration):
// a NON-generic class's `owned` field, initialized via `new`, replayed
// through the ALREADY-correct `bz$<Type>$init` mechanism this fix does NOT
// touch (records cannot have `owned` fields at all, §9.7 — "Records cannot
// have destructors, `owned` fields, or implement `IDisposable`" — so the
// owned-field shape can only be a class). This is a regression guard: this
// lane widens ModuleMetadataBuilder.cpp's Record-side gates only, and this
// row confirms the class-side `owned` field disposal balance (#2452/#2874's
// existing mechanism) is unperturbed, looping 200x so a per-iteration leak
// (which a one-shot check cannot observe, LANE_RULES §3) would accumulate
// and fail under LSan.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t9_non_generic_class_owned_field_disposal_balanced_under_asan) {
    namespace fs = std::filesystem;
    std::string stem = unique_stem("bz3723_t9_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723Owned; "
        "public class Cell3723Owned public tag: int := 3; end Cell3723Owned; "
        "public class NGOwned3723 "
        "  public owned cell: Cell3723Owned := new Cell3723Owned(); "
        "end NGOwned3723;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = fs::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b_asan")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T9", "Lib3723Owned",
                   "    var bad := 0;\n"
                   "    var k: int := 0;\n"
                   "    while k < 200 do\n"
                   "      do\n"
                   "        using var g := new NGOwned3723();\n"
                   "        if g.cell = null then bad := bad + 1; end if;\n"
                   "        if g.cell.tag <> 3 then bad := bad + 1; end if;\n"
                   "      end do;\n"
                   "      k := k + 1;\n"
                   "    end;\n"
                   "    return bad;\n"));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    // Build under ASan/LSan (BEZEL_AOT_FORCE_SANITIZE, tests/README.md's
    // documented switch — same idiom as xmod_imported_generic_field_init_2874
    // T9): a per-iteration disposal imbalance accumulates over 200 loops and
    // is caught at process exit by LSan; the plain (non-ASan) oracle cannot
    // see a leak at all (LANE_RULES §R12-G).
    setenv("BEZEL_AOT_FORCE_SANITIZE", "address", /*overwrite=*/1);
    bool built = driver.compileFile(bSrc.path, bExe.path, deps).success;
    unsetenv("BEZEL_AOT_FORCE_SANITIZE");
    BEZEL_EXPECT_TRUE(built);
    int status = std::system(bExe.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0);
}

// ===========================================================================
// T10 — REVIEW FOLLOW-UP (blocking finding on this commit's first version):
// a NON-GENERIC record's STATIC field must stay on the "external storage
// global" mechanism (§22.12 rule 1) — its initializer runs ONCE in the
// DECLARING module's own compilation, where an `internal` sibling IS
// visible, and the importer just reads the resulting global by reference.
// The first version of this fix's FieldDecl-branch gate had no
// `!isStaticField` exclusion, so a static field's `fieldInitText` got
// populated even when the initializer references something not visible
// across the boundary — turning what should be a working external-global
// read into a HARD §2.8.2 import-time reject (ModuleLoader.cpp's
// parseAndAttachFieldInit isStatic=true reject path). This row is the
// measured regression pin: it references an INTERNAL sibling specifically
// because that shape is uncarriable even in principle (the sibling is not
// part of the exported surface) — so if `fieldInitText` were populated for
// it, this would FAIL TO COMPILE instead of running.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t10_non_generic_record_static_field_internal_initializer_still_compiles_22_12) {
    std::string stem = unique_stem("bz3723_t10_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723StaticReject; "
        "internal class Secret3723Static "
        "  internal static method Get(): int => 88; "
        "end Secret3723Static; "
        "public record R3723StaticReject "
        "  public static X: int := Secret3723Static.Get(); "
        "end R3723StaticReject;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T10", "Lib3723StaticReject",
                   "    return R3723StaticReject.X;\n"));
    // Must COMPILE and RUN (the external-global mechanism), reading the
    // exporter's own correctly-computed value — not a §2.8.2 reject.
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 88);
}

// ===========================================================================
// T11 — parity: a NON-GENERIC record's static field with a RECONSTRUCTIBLE
// (public-sibling-referencing) initializer reads correctly too — via the
// SAME external-global mechanism as T10, not via `fieldInitText` replay
// (this fix's static-field exclusion applies uniformly, whether or not the
// initializer happens to be reconstructible; a static field never routes
// through the text-carriage/replay path for a non-generic record at all).
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t11_non_generic_record_static_field_public_initializer_reads_correctly_22_12) {
    std::string stem = unique_stem("bz3723_t11_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723StaticOk; "
        "public class Pub3723Static "
        "  public static method Get(): int => 55; "
        "end Pub3723Static; "
        "public record R3723StaticOk "
        "  public static X: int := Pub3723Static.Get(); "
        "end R3723StaticOk;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T11", "Lib3723StaticOk",
                   "    return R3723StaticOk.X;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3723(bSrc.path, bExe.path, aObj), 55);
}

// ===========================================================================
// T12 — CONTROL: a GENERIC record's static field with an uncarriable
// (internal-sibling) initializer is UNCHANGED by this fix's static-field
// exclusion — the `isGenericType` arm still carries the text regardless of
// `isStaticField` (per #2860/#2871: each consumer's compilation
// independently monomorphizes its own copy of per-instantiation static
// state, #1513's LinkOnceODR-shared cell, so the exporter's own
// compilation cannot run the initializer on the consumer's behalf and the
// text MUST cross so the importer can reconstruct it). This still rejects,
// exactly as it did before this review follow-up and before #3723 itself.
// ===========================================================================
BEZEL_TEST(xmod_non_generic_record_field_init_replay_3723,
           t12_generic_record_static_field_internal_initializer_still_rejects_control_2860) {
    std::string stem = unique_stem("bz3723_t12_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3723(stem,
        "module Lib3723GenStaticReject; "
        "internal class Secret3723GenStatic "
        "  internal static method Get(): int => 88; "
        "end Secret3723GenStatic; "
        "public record R3723GenStaticReject<T>(a: T) "
        "  public static X: int := Secret3723GenStatic.Get(); "
        "end R3723GenStaticReject;",
        aObj));
    TempFile3723 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3723 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3723 bExe((dir / (stem + "_b")).string());
    writeSource3723(bSrc.path,
        appSrc3723("App3723T12", "Lib3723GenStaticReject",
                   "    return R3723GenStaticReject<int>.X;\n"));
    std::string err;
    bool ok = compileImportCaptureErr3723(bSrc.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot import static field") != std::string::npos);
}

}  // namespace bezel::module
