// fix #3671 (§9.1 / §9.2 / §22.8 / §2.8.2, v0.286) — census row 7 residual
// (docs/proposals/papers/duplication-census-2026-08-30.md §2 row 7 / §6 point 6):
// `buildRecordValue` (LlvmBackend_Exprs2.cpp) initialized an imported record's
// body-declared field/auto-property with ONLY the constant-foldable-text
// replay (`ctx_.importedConstValue` / `foldImportedConstText`, #2467/#328),
// silently leaving the field at its §7.9 zero default for any NON-foldable
// initializer (a call, a `new`, an interpolation, a member read, a
// conditional). `emitOwnFieldInit` (LlvmBackend_Setup.cpp), the CLASS-side
// twin, already tries the FULL parsed-AST-expr replay first
// (`ctx_.importedInstanceFieldInitExpr`, #2874) before falling back to the
// same constant fold — two spellings of "what is this imported field's
// initial value", disagreeing on every non-foldable shape.
//
// THE FIX (this commit): `buildRecordValue`'s body-init loop now ALSO tries
// `ctx_.importedInstanceFieldInitExpr(m)` first, exactly where
// `emitOwnFieldInit` does, before its existing fold-only fallback — reusing
// the loop's own `if (init)` block (§5.1 widening / §23.1 nullable-value
// wrap) rather than duplicating it, and deliberately NOT reusing
// `emitOwnFieldInit`'s class-only retain/box/§29.7-wrap logic (§9/§9.7: a
// record's reference fields are borrows with no disposer).
//
// ⚠ SCOPE — THIS DOES NOT FULLY CLOSE #3671. MEASURED root cause, on a real
// two-module build:
//
//   ModuleMetadataBuilder.cpp's `isGenericMonomorphized` gate (#2871) only
//   populates the wire's `fieldInitText` (and therefore
//   `importedInstanceFieldInitExpr`) for a GENERIC record's (or generic
//   class's) instance field. A NON-GENERIC record's non-constant field
//   initializer — #3671's own stated repro shape, `public record R(a: int) {
//   public b: int := Helper(); }` with NO generic parameter — is not carried
//   across the module boundary AT ALL: `convertMember`'s FieldDecl branch
//   (ModuleMetadataBuilder.cpp ~line 1075) gates `out.fieldInitText` on
//   `isGenericType` alone, unlike the ConstructorSymbol branch's
//   `ctorBodyText` (~line 1283), which fix #2525 already widened to
//   `(isGenericType || Record)` for the identical "a record has no
//   cross-module call to replay an initializer through" reason. Extending
//   that same widening to the FieldDecl (and Property auto-initializer)
//   branches is a WRITER-side change in a file this lane does not own
//   (`ModuleMetadataBuilder.cpp`/`ModuleLoader.cpp` are outside
//   `src/codegen/LlvmBackend_Exprs2.cpp` + `LlvmBackend_impl.hpp`) — filed as
//   #3723. T6 below pins the non-generic shape as an unchanged, documented
//   control, not a silently-introduced regression: `buildRecordValue` cannot
//   observe an initializer the wire never carried, so it correctly falls
//   through to the same §7.9-default silence as before this fix (§6c of this
//   run's lane rules: no signal reaching codegen, no loud reject possible
//   either — that needs #3723 first).
//
// This is therefore a `narrow #3671` fix: it closes the GENERIC half of the
// duplication (every non-foldable shape now replays for a generic record,
// exactly as it already did for a generic class) and leaves the non-generic
// half open, tracked by #3723, with zero behavioral change to that shape.
//
// UPDATE (fix #3723, a later commit in this file's history): #3723 has since
// landed — it widened `ModuleMetadataBuilder.cpp`'s FieldDecl branch (and a
// second, independently-drifting copy of the same gate in the private/
// layout-only-field carriage) exactly as the paragraph above predicted. T6
// below now pins the FIXED, non-zero value instead of the documented gap;
// see tests/module/xmod_non_generic_record_field_init_replay_3723_test.cpp
// for the fuller regression grid #3723 shipped (call/new/member-read/
// conditional/private-field/loud-reject rows, plus a non-generic-class
// control confirming that path was already correct and is untouched).
//
// §6b exclusion diff — what the fold-only copy zeroed, and what the new
// parsed-expr-replay path now admits, for a GENERIC imported record:
//   - call                 (`H.Get()`)                — was silently 0, now replays (T1)
//   - `new`                (`new Pt(3, 4)`)            — was silently 0, now replays (T2)
//   - static member read   (`H.Val`)                   — was silently 0, now replays (T3)
//   - conditional           (`(if H.Get() > 0 then 1 else 2)`) — was silently 0, now replays (T4)
//   - interpolation        (`$"v{H.Get()}"`)           — was silently WRONG on BOTH the
//     record path and its class twin when THIS commit landed; FIXED later by
//     #3724, so T5 below has been FLIPPED from pinning the known gap (`2`, the
//     null-like answer) to pinning the correct replayed value (`0`, "v42") on
//     both legs. #3724's root cause was NOT in this replay: the parser stamped
//     a ZERO-WIDTH span on `ast::InterpolatedStringExpr`, so
//     ModuleMetadataBuilder's `fieldInitText` slice — guarded by
//     `span.begin.offset < span.end.offset` — never put the initializer on the
//     wire at all, and neither replay arm (nor the #2860 loud reject, gated on
//     that same text) could see it. The parity this row pins is therefore now
//     parity at the CORRECT value.
//
// FAIL-WITHOUT / PASS-WITH (measured on this lane's worktree, #3671's own
// commit): reverting `src/codegen/LlvmBackend_Exprs2.cpp` to its parent
// commit and relinking `bezelc` makes T1-T4 and T7 fail (T1/T2/T3/T4 read
// back 0/the §7.9 default instead of the replayed value; T7's diagnostic
// never fires because the fold-only path takes a silent `continue`). T8
// passes unconditionally (it pins behavior this fix does not change). T6's
// own FAIL-WITHOUT/PASS-WITH (post-#3723) is recorded at #3723's commit —
// see the writer-side test file named above.

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

struct TempFile3671 {
    std::string path;
    explicit TempFile3671(std::string p) : path(std::move(p)) {}
    ~TempFile3671() { std::remove(path.c_str()); }
};

void writeSource3671(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool buildLib3671(const std::string& stem, const std::string& libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource3671(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src, objOut).success;
    std::remove(src.c_str());
    return ok && std::ifstream(objOut).good();
}

// Compile B (importing A) -> exe, run, report exit (-1 on any compile/link/
// run failure, matching the sibling #2467/#2874 test files' convention).
int compileImportAndRun3671(const std::string& bSrc, const std::string& bExe,
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

// Same shape as #2874's diagnostic helper: compile B importing A, capturing
// stderr, so a test can assert the diagnostic TEXT and LOCATION rather than
// just "it failed".
bool compileImportCaptureErr3671(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() / ("bz3671_err_" + unique_stem("") + ".txt"))
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

// The library shared by T1-T5: a GENERIC record whose body fields cover the
// four shapes the fold-only copy zeroed and now replays (call/new/member
// read/conditional), plus the interpolation-parity fixture (both a class and
// a record twin, so T5 can compare them directly).
const char* kLib3671 =
    "module Lib3671; "
    "public class H3671 "
    "  public static Val: int := 11; "
    "  public static method Get(): int => 42; "
    "end H3671; "
    "public record Pt3671(x: int, y: int) "
    "end Pt3671; "
    "public record R3671<T>(a: T) "
    "  public bCall: int := H3671.Get(); "
    "  public bNew: Pt3671 := new Pt3671(3, 4); "
    "  public bMember: int := H3671.Val; "
    "  public bCond: int := (if H3671.Get() > 0 then 1 else 2); "
    "end R3671; "
    "public record RInterp3671<T>(a: T) "
    "  public bInterp: string := $\"v{H3671.Get()}\"; "
    "end RInterp3671; "
    "public class GInterp3671<T> "
    "  public v: T; "
    "  public bInterp: string := $\"v{H3671.Get()}\"; "
    "  public constructor(v0: T) v := v0; end constructor; "
    "end GInterp3671; "
    "public record R3671NonGeneric(a: int) "
    "  public b: int := H3671.Get(); "
    "end R3671NonGeneric;";

std::string appSrc3671(const std::string& moduleName, const std::string& lib,
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
// T1 — CALL shape: `public bCall: int := H3671.Get();`. Pre-fix this read `0`
// (foldImportedConstText's renderConstExpr returns "" for a call). Post-fix
// it replays the exporter's declared `42`.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t1_generic_record_call_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3671_t1_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T1", "Lib3671",
                   "    var r := new R3671<int>(9);\n"
                   "    return r.bCall;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3671(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T2 — `new` shape: `public bNew: Pt3671 := new Pt3671(3, 4);`. Pre-fix
// `bNew` read the zero-valued `Pt3671(0, 0)`. Post-fix it replays the
// exporter's declared `Pt3671(3, 4)`.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t2_generic_record_new_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3671_t2_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T2", "Lib3671",
                   "    var r := new R3671<int>(9);\n"
                   "    return r.bNew.x * 10 + r.bNew.y;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3671(bSrc.path, bExe.path, aObj), 34);
}

// ===========================================================================
// T3 — static MEMBER READ shape: `public bMember: int := H3671.Val;`.
// Pre-fix `bMember` read `0`. Post-fix it replays the exporter's declared
// `11`.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t3_generic_record_member_read_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3671_t3_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T3", "Lib3671",
                   "    var r := new R3671<int>(9);\n"
                   "    return r.bMember;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3671(bSrc.path, bExe.path, aObj), 11);
}

// ===========================================================================
// T4 — CONDITIONAL shape: `public bCond: int := (if H3671.Get() > 0 then 1
// else 2);`. Pre-fix `bCond` read `0`. Post-fix it replays the exporter's
// declared `1` (the condition is true at the exporter's own declared value).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t4_generic_record_conditional_initializer_replays_9_1_22_8) {
    std::string stem = unique_stem("bz3671_t4_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T4", "Lib3671",
                   "    var r := new R3671<int>(9);\n"
                   "    return r.bCond;\n"));
    BEZEL_EXPECT_EQ(compileImportAndRun3671(bSrc.path, bExe.path, aObj), 1);
}

// ===========================================================================
// T5 — INTERPOLATION PARITY, **FLIPPED by fix #3724**. When #3671 landed,
// both a GENERIC RECORD and a GENERIC CLASS field initialized with
// `$"v{H3671.Get()}"` read back a value that was neither the exporter's
// declared "v42" NOR a valid empty string: a NULL pointer parked in the
// non-nullable `string` slot. `Console.WriteLine` and `=` have their own null
// guards (so the comparison below fell through to `return 2` with no crash),
// while `.Length` and string `+` SEGFAULTed deterministically, 5/5 runs, on
// BOTH legs — they emit no null guard precisely because a non-nullable slot is
// assumed by construction never to hold null. This row pinned that shared
// wrong answer as `2`: parity achieved, correctness not.
//
// #3724 has since FIXED it, and its root cause was NOT the shared #2874
// load-time reconstruction this comment previously suspected. MEASURED: the
// parser stamped a ZERO-WIDTH span on `ast::InterpolatedStringExpr` — the
// branch pre-stamps `span.begin`/`span.end` at the same location so
// `splitInterpolation`'s own diagnostics have somewhere to point, which also
// makes `parsePrimaryExpression`'s `stampSpan` wrapper a no-op — so
// ModuleMetadataBuilder's `fieldInitText` slice, guarded by
// `span.begin.offset < span.end.offset`, never put the initializer on the wire
// at all. Neither replay arm had anything to reconstruct, and the #2860 loud
// reject, gated on that same text, could not fire either. The reconstruction
// machinery was always capable of replaying an interpolation: the SAME
// initializer wrapped in a binary `+` (`$"v{H3671.Get()}" + ""`) replayed to
// the correct "v42" even before #3724.
//
// The row therefore still pins PARITY — the actual scope of #3671, two paths
// answering the SAME question the SAME way — but now at the CORRECT shared
// answer (`0`, i.e. "v42"). A regression that reintroduces the null shows up
// here as `2` on both legs; a class/record divergence shows up as the parity
// assertion failing. See
// tests/module/xmod_imported_field_init_interpolation_3724_test.cpp for
// #3724's own grid (the verbatim / formatted / multi-hole / hole-free shapes,
// the non-generic class and record legs, the `.Length`/`+` crash surface, and
// a 200-iteration ASan/LSan dispose-exactly-once row).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t5_interpolation_shape_parity_with_class_twin_fixed_3724) {
    std::string stem = unique_stem("bz3671_t5_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    // 0 -> "v42" (correct — the post-#3724 answer), 1 -> "" (a valid empty
    // string), 2 -> neither literal matched (the pre-#3724 null-like answer).
    TempFile3671 bSrcR((dir / (stem + "_r.bzl")).string());
    TempFile3671 bExeR((dir / (stem + "_r")).string());
    writeSource3671(bSrcR.path,
        appSrc3671("App3671T5R", "Lib3671",
                   "    var r := new RInterp3671<int>(9);\n"
                   "    if r.bInterp = \"v42\" then return 0; end if;\n"
                   "    if r.bInterp = \"\" then return 1; end if;\n"
                   "    return 2;\n"));
    int recordResult = compileImportAndRun3671(bSrcR.path, bExeR.path, aObj);

    TempFile3671 bSrcC((dir / (stem + "_c.bzl")).string());
    TempFile3671 bExeC((dir / (stem + "_c")).string());
    writeSource3671(bSrcC.path,
        appSrc3671("App3671T5C", "Lib3671",
                   "    var g := new GInterp3671<int>(9);\n"
                   "    if g.bInterp = \"v42\" then return 0; end if;\n"
                   "    if g.bInterp = \"\" then return 1; end if;\n"
                   "    return 2;\n"));
    int classResult = compileImportAndRun3671(bSrcC.path, bExeC.path, aObj);

    // Parity is the property #3671 owns: record and class must agree.
    BEZEL_EXPECT_EQ(recordResult, classResult);
    // Fixed shared answer (fix #3724): 0 — both legs read the exporter's
    // declared "v42". This was 2 (neither literal matched: the null-like
    // answer) until #3724 closed the metadata-carriage gap.
    BEZEL_EXPECT_EQ(recordResult, 0);
}

// ===========================================================================
// T6 — FLIPPED by fix #3723 (was a documented known-gap control pinning the
// WRONG value `0`; now pins the FIXED value `42`): a NON-GENERIC imported
// record's non-constant field initializer now replays exactly like the
// GENERIC case above. #3723 widened `ModuleMetadataBuilder.cpp`'s FieldDecl
// branch gate from `isGenericType` alone to `(isGenericType || Record)`
// (mirroring fix #2525's identical widening of the ConstructorSymbol
// branch's `ctorBodyText`), so `ctx_.importedInstanceFieldInitExpr` now
// returns the reconstructed expression for this shape too, and
// `buildRecordValue`'s #3671 replay (this file's own fix, unchanged by
// #3723) consumes it with no further codegen change. This is #3671's OWN
// stated repro shape (`public record R(a: int) { public b: int :=
// Helper(); }`) — see
// tests/module/xmod_non_generic_record_field_init_replay_3723_test.cpp for
// the fuller #3723 regression grid (call/new/member-read/conditional/
// private-field/loud-reject rows).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t6_non_generic_record_call_initializer_now_replays_3723) {
    std::string stem = unique_stem("bz3671_t6_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem, kLib3671, aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T6", "Lib3671",
                   "    var r := new R3671NonGeneric(9);\n"
                   "    return r.b;\n"));
    // Fixed answer (fix #3723): 42, the exporter's declared value.
    BEZEL_EXPECT_EQ(compileImportAndRun3671(bSrc.path, bExe.path, aObj), 42);
}

// ===========================================================================
// T7 — LOUD REJECT (§2.8.2 / §6c of this run's lane rules): a GENERIC record
// whose field initializer references an INTERNAL sibling (visible to the
// exporter's own compile, not part of the exported public surface, §13.2) —
// the load-time reconstruction fails to resolve it, the constant fold also
// fails (a call is not foldable), and `importedInstanceFieldInitText` IS
// present (the exporter DID carry the fact "this field has an initializer"
// for this GENERIC type) — so this fix's new diagnostic fires instead of
// silently leaving the field at its zero default. Mirrors
// `xmod_imported_generic_field_init_diag_2874_test.cpp`'s own fixture shape,
// applied to a RECORD instead of a class.
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t7_uncarriable_generic_record_initializer_rejects_loudly_2_8_2) {
    std::string stem = unique_stem("bz3671_t7_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib3671(stem,
        "module Lib3671Reject; "
        "internal class Secret3671 "
        "  internal static method Get(): int => 7; "
        "end Secret3671; "
        "public record R3671Reject<T>(a: T) "
        "  public b: int := Secret3671.Get(); "
        "end R3671Reject;",
        aObj));
    TempFile3671 aObjFile(aObj);

    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3671 bExe((dir / (stem + "_b")).string());
    writeSource3671(bSrc.path,
        appSrc3671("App3671T7", "Lib3671Reject",
                   "    var r := new R3671Reject<int>(9);\n"
                   "    return r.b;\n"));
    std::string err;
    bool ok = compileImportCaptureErr3671(bSrc.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot run the initializer of its field") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`b`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("R3671Reject") != std::string::npos);
}

// ===========================================================================
// T8 — CONTROL: a fully LOCAL generic record (no import at all) with the
// SAME non-foldable-initializer shape as T1. Guards against this fix's new
// `ctx_.importedInstanceFieldInitExpr` lookup affecting same-module
// compilation — a local symbol always has `m->decl != nullptr`, so the new
// `if (!init && m->decl == nullptr)` guard must keep this path byte-for-byte
// unchanged (mirrors #2467's own T6).
// ===========================================================================
BEZEL_TEST(xmod_record_body_field_init_replay_generic_3671,
           t8_local_generic_record_unaffected_control) {
    bezel::Driver driver;
    std::string stem = unique_stem("bz3671_t8_");
    auto dir = std::filesystem::temp_directory_path();
    TempFile3671 src((dir / (stem + ".bzl")).string());
    TempFile3671 exe((dir / stem).string());
    writeSource3671(src.path,
                    "module LocalCtl3671; "
                    "public class HL3671 "
                    "  public static method Get(): int => 42; "
                    "end HL3671; "
                    "public record LR3671<T>(a: T) "
                    "  public b: int := HL3671.Get(); "
                    "end LR3671; "
                    "public class C3671T8 "
                    "  public static method Main(): int "
                    "    var r := new LR3671<int>(9); "
                    "    return r.b; "
                    "  end Main; "
                    "end C3671T8;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
