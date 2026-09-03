// fix #3736 (§9.2 / §22.8 step 2 / §2.8.2, v0.286): a record/class
// AUTO-PROPERTY's non-foldable initializer (`property W: int := <call
// expr>;`) was never carried across the module boundary AT ALL — the
// PropertySymbol branch of `ModuleMetadataBuilder.cpp`'s `convertMember`
// had NO `fieldInitText`-equivalent export, for either a generic or a
// non-generic record, public or private. This is the Property twin of the
// gap fix #3723 (34558be) closed for FieldDecl — #3723's own commit body
// filed this exact gap as #3736, "out of that issue's own stated scope".
//
// Measured repro (issue #3736's own, verified on this lane's HEAD before
// this fix): a fully PUBLIC, NON-generic record —
//   public class H  public static method Get(): int => 42; end H;
//   public record R(a: int)  public property W: int := H.Get();  end R;
// — imported and constructed (`new R(9).W`) read `0`, not `42`, with NO
// diagnostic at any severity (§2.8.2 requires either the correct replayed
// value or a diagnostic naming the shape; the pre-fix implementation gave
// neither).
//
// THE FIX (src/module/ModuleMetadataBuilder.cpp, PropertySymbol branch):
// populate `out.fieldInitText` for a non-static AUTO-property's initializer
// — reusing the EXACT wire slot #3723 widened for FieldDecl (no new
// metadata surface, mirroring #2467's own precedent of reusing `valueText`
// for Property) — gated `!isStaticProp && (isGenericType || isRecordType)`.
// This is Field's `(isGenericType || (isRecordType && !isStaticField))`
// gate with ONE deliberate, documented difference: Field carries for a
// GENERIC type's STATIC field unconditionally (the already-working
// #2351/#2860 static-field reconstruction path); no such reconstruction
// path exists for a Property yet, so this fix's gate excludes ALL static
// properties, generic or not (a GENERIC type's STATIC auto-property stays
// on the pre-existing external-storage-global story, unaffected — a
// separate, unfiled gap, out of THIS issue's scope, whose own repro and
// this brief's §6b shape list are both about instance-position
// auto-properties).
//
// READER SIDE (src/module/ModuleLoader.cpp), needed because the driving
// loop and `parseAndAttachFieldInit` were Field-only:
//   1. The `mm.kind != MetaMemberKind::Field` driving-loop gate widened to
//      also admit `MetaMemberKind::Property` (never Indexer — an indexer
//      gets its own MetaMemberKind, so `kind == Property` alone already
//      excludes it), looking up a non-indexer PropertySymbol by name
//      instead of a FieldSymbol when the target is a Property.
//   2. `parseAndAttachFieldInit`'s `sema::FieldSymbol* importedFs`
//      parameter widened to `sema::Symbol*` (works for BOTH — every other
//      step: type re-resolution, sibling-type registration, lambda-param /
//      pattern-symbol copy, is untouched and identical for both), with the
//      member LOOKUP and INITIALIZER EXTRACTION branched by `target.kind`
//      (`FieldDecl::initializer` vs `PropertyDecl::autoInitializer`).
//   3. `buildSyntheticFieldInitSource` widened to also re-declare each
//      AUTO-property member carrying its own `fieldInitText` as
//      `property Name : Type := <text>;`, mirroring the Field stub loop —
//      so a self/sibling storage reference in an initializer resolves.
//   4. The self-storage remap map (`importedFields`, Field-only before)
//      widened to also index a non-indexer PropertySymbol, and the
//      exprSymbol remap loop's `dynamic_cast<FieldSymbol*>` condition
//      widened to also catch `PropertySymbol` — defensive symmetry with
//      the Field path; §22.8's admitted-initializer-reference list ("static
//      members of any class... primary-constructor parameters... typeof
//      expressions") does not admit a self-INSTANCE-storage reference at
//      all, so this widening has no positive test here (mirrors how the
//      pre-existing Field self-remap is itself mostly dead for the same
//      reason — meaningfully exercised only by the pre-existing static-to-
//      static #2351 mechanism, which this fix does not touch).
//
// CODEGEN (LlvmBackend_Exprs2.cpp `buildRecordValue`, LlvmBackend_Setup.cpp
// `emitOwnFieldInit`) needed NO change — confirmed by reading both:
//   * `buildRecordValue` (record construction) already branches on
//     `dynamic_cast<const ast::PropertyDecl*>(m->decl)` for the local case
//     and reads `ctx_.importedInstanceFieldInitExpr(m)` /
//     `importedInstanceFieldInitText(m)` generically (keyed on the member
//     `Symbol*`, never FieldSymbol-typed) for the imported decl-null
//     fallback — exactly as issue #3736's own text predicted.
//   * `emitOwnFieldInit` (generic-class monomorphized construction) already
//     collects `sema::isAutoPropertyStorageSymbol(m, ctx_)` into its own
//     `storage` walk (fix #3152) and reads the SAME
//     `importedInstanceFieldInitExpr` / `importedConstValue` /
//     `importedInstanceFieldInitText` side tables for a Property member as
//     for a Field — #3152's own commit measured this exact class of bug
//     for a GENERIC class's auto-property (`Box<T>` with
//     `property W: int := 42;` read `7` instead of `49`) and fixed the
//     CONSUMER; #3736 is the missing WRITER half for that same generic-
//     class shape, plus the record shape #3152 does not cover at all
//     (records go through `buildRecordValue`, not `emitOwnFieldInit`).
//
// §6b exclusion diff (verified on real two-module builds, this file):
//   - non-generic record, PUBLIC auto-property, call init      -- was 0
//     with NO diagnostic, now replays 42 (T1, issue's own repro)
//   - non-generic CLASS, auto-property, call init              -- ALREADY
//     correct via `bz$<Type>$init` (#2452); UNCHANGED by this fix (T2)
//   - COMPUTED property (`=> expr`, no backing field)          -- never
//     exported (no `pd->bodyKind == Auto` gate ever fires for it);
//     UNCHANGED, still computed live on every read (T3)
//   - STATIC auto-property (record), uncarriable init          -- UNCHANGED
//     end-to-end (this fix's UNCONDITIONAL `!isStaticProp` exclusion,
//     stricter than Field's generic-carries-anyway rule — see rationale
//     above — means this never even reaches the text-carriage/reject path);
//     MEASURED while writing T4 (mirrors #3723's T10 in INTENT — "does this
//     fix newly reject a static property?" — no): a static auto-property's
//     NON-FOLDABLE initializer never ran AT ALL, even single-module with no
//     import, a pre-existing gap this fix's own exclusion cannot be the
//     cause of and did not touch — filed as issue #3749. UPDATE (fix #3749,
//     landed after this fix, §23.1/§22.13): #3749 fixed the initializer at
//     its actual source — the exporting module's OWN static-initializer
//     emission (LlvmBackend_Setup.cpp's sfinit function, not this fix's
//     metadata-carriage gate, which is untouched and stays UNCONDITIONALLY
//     `!isStaticProp`) — so `R3736T4.X` now reads `88` cross-module too, via
//     the pre-existing #798 mechanism (an imported public type's static
//     initialization runs, on first cross-module use, as compiled code in
//     ITS OWN defining module — no AST/text carriage needed, unlike an
//     instance auto-property's construction-time replay). T4 below now
//     asserts the value directly instead of only "not -1"

//   - GENERIC record, auto-property, call init                 -- was 0,
//     now replays (T5)
//   - GENERIC class, auto-property, call init                  -- was 7
//     instead of 49 (#3152-shaped silent-wrong-data bug), now replays
//     correctly (T6)
//   - non-generic record, PRIVATE/layout-only auto-property, call init,
//     read via public method                                   -- was 0,
//     now replays (T7, mirrors Field's #3723 T6)
//   - non-generic record, auto-property referencing an INTERNAL sibling
//     (genuinely uncarriable)                                  -- was
//     silently 0 with NO diagnostic; now a loud §2.8.2 reject naming the
//     field, at the construction site (T8, mirrors Field's #3723 T7)
//   - a fully LOCAL (non-imported) record auto-property         -- UNCHANGED
//     (T9)
//   - a STRING-valued auto-property (ARC-relevant), imported, constructed
//     200x under ASan/LSan                                      -- was 0 /
//     potentially leak/double-free-prone via the newly-exercised replay
//     path; now correct and balanced (T10)
//
// FAIL-WITHOUT / PASS-WITH: reverted `src/module/ModuleMetadataBuilder.cpp`
// and `src/module/ModuleLoader.cpp` to their parent commit (153fc26, this
// lane's base) in this worktree, relinked `bezelc`, and confirmed T1, T5,
// T6, T7, T8 fail (T1/T5/T6/T7 read back `0`/the wrong pre-#3152-fix value
// instead of the exporter's declared value; T8's diagnostic never fires —
// compile succeeds instead of rejecting) while T2, T3, T4, T9, T10 still
// pass (unaffected controls). Restored the tree and relinked the fixed
// `bezelc` before any further measurement (LANE_RULES §R12-H).
//
// Cross-module tests here use the direct `bezel::Driver` two-object-file
// idiom (matching #3723/#3152/#2874/#3671's own test files), NOT
// `runAotChecked` — that harness compiles a single self-contained program
// and has no dependency-object-file parameter, so it cannot express a
// cross-module import. T10's ASan/LSan leg uses the same
// `BEZEL_AOT_FORCE_SANITIZE=address` idiom #3723's own T9 uses.

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

struct TempFile3736 {
    std::string path;
    explicit TempFile3736(std::string p) : path(std::move(p)) {}
    ~TempFile3736() { std::remove(path.c_str()); }
};

void writeSource3736(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool buildLib3736(const std::string& stem, const std::string& libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource3736(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src, objOut).success;
    std::remove(src.c_str());
    return ok && std::ifstream(objOut).good();
}

// Compile B (importing A) -> exe, run, report exit (-1 on any compile/link/
// run failure, matching the sibling #2467/#2874/#3671/#3723 test files'
// convention).
int compileImportAndRun3736(const std::string& bSrc, const std::string& bExe,
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

// Same shape as #2874/#3671/#3723's diagnostic helper: compile B importing
// A, capturing stderr, so a test can assert the diagnostic TEXT rather than
// just "it failed".
bool compileImportCaptureErr3736(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() / ("bz3736_err_" + unique_stem("") + ".txt"))
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

std::string appSrc3736(const std::string& moduleName, const std::string& lib,
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
// T1 — issue #3736's own measured repro: a NON-generic, fully PUBLIC record
// whose auto-property initializer is a static-method CALL (non-foldable).
// Pre-fix this read `0` with no diagnostic at any severity. Post-fix it
// replays the exporter's declared `42`.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t1_non_generic_record_auto_property_call_initializer_replays_9_2_22_8) {
    std::string stem = unique_stem("bz3736_t1_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T1; "
        "public class H3736T1 "
        "  public static method Get(): int => 42; "
        "end H3736T1; "
        "public record R3736T1(a: int) "
        "  public property W: int := H3736T1.Get(); "
        "end R3736T1;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T1", "Lib3736T1",
                   "    var r := new R3736T1(9);\n"
                   "    return r.W;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T2 — CONTROL: a non-generic CLASS's auto-property (identical shape to T1,
// class instead of record) was ALREADY correct before this fix — it
// replays through the exporter's own `bz$<Type>$init` (#2452), a real
// cross-module call needing no textual carriage. This fix's writer gate
// deliberately excludes a non-generic class, so its `fieldInitText` stays
// empty for this shape (inert either way — the class path never reads it).
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t2_non_generic_class_control_already_correct_unaffected) {
    std::string stem = unique_stem("bz3736_t2_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T2; "
        "public class H3736T2 "
        "  public static method Get(): int => 42; "
        "end H3736T2; "
        "public class C3736T2Ctl "
        "  public property W: int := H3736T2.Get(); "
        "end C3736T2Ctl;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T2", "Lib3736T2",
                   "    using var c := new C3736T2Ctl();\n"
                   "    return c.W;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T3 — CONTROL: a COMPUTED property (`=> expr`, no `:=`, no backing field)
// on an imported non-generic record. `pd->bodyKind` is `Expression`, not
// `Auto`, so this fix's gate (nested inside the `bodyKind == Auto` arm)
// never fires for it — `fieldInitText` stays empty, exactly as before.
// Reads the record's own positional field live on every call.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t3_computed_property_control_unaffected_no_export) {
    std::string stem = unique_stem("bz3736_t3_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T3; "
        "public record R3736T3(a: int) "
        "  public property Doubled: int => a * 2; "
        "end R3736T3;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T3", "Lib3736T3",
                   "    var r := new R3736T3(21);\n"
                   "    return r.Doubled;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T4 — CONTROL (mirrors #3723's T10 in INTENT, not in expected VALUE): a
// NON-generic record's STATIC auto-property whose initializer references an
// INTERNAL sibling — genuinely uncarriable even in principle. This fix's
// gate is UNCONDITIONALLY `!isStaticProp` (stricter than Field's "generic
// carries regardless of static" rule — see this file's header rationale),
// so `fieldInitText` never gets populated for ANY static property, and this
// program must keep COMPILING (no NEW §2.8.2 reject) — the same claim
// #3723's T10 makes for Field's static case.
//
// MEASURED while writing this test (NOT introduced by this fix, and NOT
// this fix's concern at the time): a static auto-property's NON-FOLDABLE
// initializer never actually RAN at all, even in a fully LOCAL single-module
// compilation with no import whatsoever — `R3736T4.X` read the §7.9 zero
// default regardless of whether this program was compiled standalone or
// split across modules. This was a pre-existing, SEPARATE local-codegen gap
// (static-initializer emission for a class/record's own static
// auto-property), filed as issue #3749 with a minimal single-module repro;
// this fix's own gate deliberately never carried ANYTHING at all for a
// static property (writer or reader), so it structurally could not be the
// cause.
//
// fix #3749 (landed separately, §23.1/§22.13, v0.286) closed that gap at its
// actual source: LlvmBackend_Setup.cpp's `needsRuntimeFieldInit` /
// `emitStaticConstructorBody` (the module-init static-initializer
// machinery) now also walks a static auto-property's backing global, not
// only a static FIELD's — so `Secret3736T4.Get()` now runs, in the
// EXPORTING module, at the point the pre-existing #798 mechanism already
// runs an imported public type's static initialization on first
// cross-module use (compiled code, no text/AST carriage — this fix's own
// `!isStaticProp` metadata-carriage exclusion above is untouched and stays
// exactly as documented). `R3736T4.X` now reads the exporter's declared
// `88` cross-module, exactly like its (already-working, per #1146) constant
// sibling would.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t4_static_auto_property_internal_initializer_replays_3749_22_12) {
    std::string stem = unique_stem("bz3736_t4_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T4; "
        "internal class Secret3736T4 "
        "  internal static method Get(): int => 88; "
        "end Secret3736T4; "
        "public record R3736T4 "
        "  public static property X: int := Secret3736T4.Get(); "
        "end R3736T4;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T4", "Lib3736T4",
                   "    return R3736T4.X;\n"));
    // fix #3749: was 0 (never ran) pre-#3749; now replays the exporter's
    // declared 88, cross-module, via #798's ensureinit mechanism.
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 88);
}

// ===========================================================================
// T5 — GENERIC record's auto-property, call initializer. Pre-fix `0`
// (identical gap to T1, generic side). Post-fix replays `42`.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t5_generic_record_auto_property_call_initializer_replays_9_2_22_8) {
    std::string stem = unique_stem("bz3736_t5_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T5; "
        "public class H3736T5 "
        "  public static method Get(): int => 42; "
        "end H3736T5; "
        "public record R3736T5<T>(a: T) "
        "  public property W: int := H3736T5.Get(); "
        "end R3736T5;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T5", "Lib3736T5",
                   "    var r := new R3736T5<int>(9);\n"
                   "    return r.W;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T6 — GENERIC class's auto-property, call initializer. This is #3152's
// own repro shape (that fix's commit message measured `Box<T>` with
// `property W: int := 42;` reading `7` instead of `49` pre-#3152); #3152
// fixed `emitOwnFieldInit`'s CONSUMER side but the writer never carried
// `fieldInitText` for ANY Property member at all, so a NON-foldable
// initializer here was still silently wrong pre-this-fix. Post-fix replays
// correctly.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t6_generic_class_auto_property_call_initializer_replays_9_2_22_8) {
    std::string stem = unique_stem("bz3736_t6_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T6; "
        "public class H3736T6 "
        "  public static method Get(): int => 42; "
        "end H3736T6; "
        "public class Box3736T6<T> "
        "  public property W: int := H3736T6.Get(); "
        "  private _f: int := 7; "
        "  public property Val: T; "
        "  public constructor(v: T) "
        "    Val := v; "
        "  end constructor; "
        "  public method Sum(): int => W + _f; "
        "end Box3736T6;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T6", "Lib3736T6",
                   "    using var b := new Box3736T6<int>(5);\n"
                   "    return b.Sum();\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 49);
}

// ===========================================================================
// T7 — PRIVATE/LAYOUT-ONLY auto-property (the SECOND #3723 gate's own
// Property twin, §6b): a private body auto-property, read back only
// through a public method (mirrors #2468/#3723's T6 method-marshalling
// repro). Before this fix `_hidden` read the §7.9 zero default even after
// #3723 landed (that fix only touched FieldDecl, not PropertyDecl).
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t7_private_layout_only_auto_property_replays_via_method_marshalling_9_2_22_8) {
    std::string stem = unique_stem("bz3736_t7_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T7; "
        "public class H3736T7 "
        "  public static method Get(): int => 55; "
        "end H3736T7; "
        "public record Rec3736T7(x: int) "
        "  private property _hidden: int := H3736T7.Get(); "
        "  public method GetHidden(): int => _hidden; "
        "end Rec3736T7;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T7", "Lib3736T7",
                   "    var r := new Rec3736T7(9);\n"
                   "    var h := r.GetHidden();\n"
                   "    return r.x + h;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3736(bSrc.path, bExe.path, aObj), 64);
}

// ===========================================================================
// T8 — LOUD REJECT (§2.8.2 / LANE_RULES §6c), Property twin of #3723's T7:
// a non-generic record's auto-property initializer references an INTERNAL
// sibling (not part of the exported public surface, §13.2). Load-time
// reconstruction cannot resolve it; `importedInstanceFieldInitText` IS now
// present, so `buildRecordValue`'s pre-existing #3671/79146cd diagnostic
// fires — converting the pre-#3736 SILENT §7.9-zero (no diagnostic at any
// severity, per issue #3736's own text) into a located reject.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t8_uncarriable_auto_property_initializer_rejects_loudly_2_8_2) {
    std::string stem = unique_stem("bz3736_t8_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T8; "
        "internal class Secret3736T8 "
        "  internal static method Get(): int => 7; "
        "end Secret3736T8; "
        "public record R3736T8(a: int) "
        "  public property B: int := Secret3736T8.Get(); "
        "end R3736T8;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T8", "Lib3736T8",
                   "    var r := new R3736T8(9);\n"
                   "    return r.B;\n"));
    std::string err;
    bool ok = compileImportCaptureErr3736(bSrc.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot run the initializer of its field") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`B`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("R3736T8") != std::string::npos);
}

// ===========================================================================
// T9 — CONTROL: a fully LOCAL non-generic record (no import at all), same
// shape as T1. A local record's auto-property always reads its AST
// initializer directly (`pd->autoInitializer` off a non-null `m->decl`) and
// never touches `fieldInitText` at all — guards against this fix's
// writer-side widening affecting same-module compilation.
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t9_local_non_generic_record_unaffected_control) {
    bezel::Driver driver;
    std::string stem = unique_stem("bz3736_t9_");
    auto dir = std::filesystem::temp_directory_path();
    TempFile3736 src((dir / (stem + ".bzl")).string());
    TempFile3736 exe((dir / stem).string());
    writeSource3736(src.path,
                    "module LocalCtl3736; "
                    "public class HL3736 "
                    "  public static method Get(): int => 42; "
                    "end HL3736; "
                    "public record LR3736(a: int) "
                    "  public property W: int := HL3736.Get(); "
                    "end LR3736; "
                    "public class C3736T9 "
                    "  public static method Main(): int "
                    "    var r := new LR3736(9); "
                    "    return r.W; "
                    "  end Main; "
                    "end C3736T9;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T10 — STRING-VALUED auto-property (ARC-relevant, brief's explicit
// deliverable), UNDER ASAN/LSAN, 200 ITERATIONS: a non-generic record's
// public auto-property initialized by a non-foldable call returning a
// freshly-built `string`. Constructed 200x in a loop; a per-iteration
// leak or double-free in the newly-exercised replay path (`lowerExpr` on
// the reconstructed call expression, then stored into the record's
// by-value slot) would accumulate and fail under LSan — a one-shot check
// cannot observe it (LANE_RULES §3/§R12-G).
// ===========================================================================
BEZEL_TEST(xmod_record_auto_property_field_init_replay_3736,
           t10_string_valued_auto_property_replay_balanced_under_asan) {
    namespace fs = std::filesystem;
    std::string stem = unique_stem("bz3736_t10_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3736(stem,
        "module Lib3736T10; "
        "public class Str3736T10 "
        "  public static method Make(): string => \"hello-\" + \"3736\"; "
        "end Str3736T10; "
        "public record RStr3736T10(a: int) "
        "  public property S: string := Str3736T10.Make(); "
        "end RStr3736T10;",
        aObj));
    TempFile3736 aObjFile(aObj);

    auto dir = fs::temp_directory_path();
    TempFile3736 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3736 bExe((dir / (stem + "_b_asan")).string());
    writeSource3736(bSrc.path,
        appSrc3736("App3736T10", "Lib3736T10",
                   "    var bad := 0;\n"
                   "    var k: int := 0;\n"
                   "    while k < 200 do\n"
                   "      var r := new RStr3736T10(k);\n"
                   "      if r.S.Length <> 10 then bad := bad + 1; end if;\n"
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
    // documented switch — same idiom as #3723's own T9 and #2874's T9): a
    // per-iteration leak or double-free accumulates over 200 loops and is
    // caught at process exit by LSan; the plain (non-ASan) oracle cannot
    // see a leak at all (LANE_RULES §R12-G).
    setenv("BEZEL_AOT_FORCE_SANITIZE", "address", /*overwrite=*/1);
    bool built = driver.compileFile(bSrc.path, bExe.path, deps).success;
    unsetenv("BEZEL_AOT_FORCE_SANITIZE");
    BEZEL_EXPECT_TRUE(built);
    int status = std::system(bExe.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0);
}

}  // namespace bezel::module
