// Codegen — issue #4280 / spec §15.1 ("Inline arrays") + §9.7 ("Records
// and disposal") + §28.9 ("Runtime-managed types"), docs revision v0.298.
//
// SEND-BACK for `fix #4265` — review finding 1 (SEVERE, a live UAF on
// #4265's own commit `0c45cb6`).
//
// `fix #4265` (inline_array_string_call_result_param_alias_uaf_4265_test
// .cpp, this same directory) closed the SIXTH channel of `Emitter::
// walkCallArgs`'s #1430 relaxation: a `string` local passed as a plain
// argument to a callee whose declared return type is an INLINE ARRAY of
// `string` elements is a heap-use-after-free, because the ReturnStmt
// inline-array materialization block (LlvmBackend_Stmts.cpp, the §15.3
// `return [e1, ...]` branch) stores each element with a bare
// `builder_.CreateStore(ev, gep)` — no `emitStringRetain`.
//
// That fix's own commit body listed, as explicitly out of scope: "inline
// arrays of CLASS-typed or CHOICE-typed elements (different lowering path
// entirely)". THAT CLAIM WAS WRONG, not merely unverified, for the
// RECORD/CHOICE half (the CLASS half holds — see the control row below).
// A callee returning `[N]SomeRecord` or `[N]SomeChoice` embeds the string
// argument into the array via a positional/payload constructor call whose
// OWN store is the identical raw `CreateStore` shape — `buildRecordValue`'s
// fast path, the exact mechanism `fix #4254`
// (record_choice_call_result_param_alias_uaf_4254_test.cpp) found for a
// bare `return new Str(s);` — one level removed, through the array slot.
// This is the SEVENTH channel of the #4254/#4260/#4265 family, filed as
// #4280 and closed here.
//
// GENERALIZED, not enumerated as an eighth/ninth special case:
// `LlvmBackend_Setup.cpp`'s `calleeReturnMayAliasArgUnretained` guard now
// shares ONE `isRecordOrChoiceDeclared` test between the top-level `crt`
// arm (`fix #4254`) and a new `elementMayEmbedStringArgUnretained` helper
// applied to `crt->arrayElement` (covering both `fix #4265`'s
// Primitive/String element case and this fix's Record/Choice element
// case). This is bounded to ONE level of nesting, not open-ended
// recursion: §15.1 states outright "Inline arrays may not be elements of
// inline arrays in the form `[N][M]T` ... The current spec admits only
// one-dimensional inline arrays" — `crt->arrayElement` can never itself be
// `Kind::InlineArray`, so there is no possible eighth, doubly-nested
// channel this generalization would need to reach.
//
// NOT generalized to "any Named `arrayElement`": a CLASS-typed element is
// deliberately excluded. A class's own constructor retains a borrowed
// string field via `lowerStringFieldStore` (unlike a record's/choice's raw
// positional/payload store), so the #1430 relaxation's soundness argument
// actually holds for a class-element array — suppressing the caller-side
// release there would be pure OVER-suppression (an unforced extra leak,
// not a fix). The `inline_array_owned_class_element_control_stays_clean_
// not_over_suppressed` row below pins this boundary directly: it is the
// InlineArray-element analogue of #4254's own
// `class_returning_factory_control_stays_clean_spec_28_3` row.
//
// MEASURED FAIL-WITHOUT (this send-back's own worktree, isolated —
// `scripts/agent_check.sh --probe --asan FILE.bzl` against `0c45cb6`
// itself, i.e. WITH fix #4265 already applied and WITHOUT this fix):
//   - RECORD element (`[1]Str`, `record Str(V: string)`): a real
//     `AddressSanitizer: heap-use-after-free` — READ of size 8, freed in
//     `main`, previously allocated in `main`, the identical 32-byte
//     `bz.string`-header region signature every other row in this family
//     measures.
//   - CHOICE element (`[1]Tag`, `Tag.Named(s)`): the identical
//     heap-use-after-free signature.
// MEASURED PASS-WITH (this fix, same rebuild-and-rerun cycle): both clean,
// exit 5 (`"hello".Length`), no ASan/LSan output at all.
// So 2 of 2 UAF rows (named above) fail without the fix and pass with it.
// The CLASS control row is clean on BOTH sides (measured pre-fix on
// `0c45cb6` and post-fix on this commit) — it is a boundary pin, not a
// FAIL-WITHOUT row, by construction (the generalization deliberately does
// not touch this shape).
//
// IMPORTED AXIS: reuses `fix #4260`'s `LibHandle`/`AppHandle` in-process
// two-module harness (`Driver::compileObject`/`compileFile` with
// `CodegenOptions.sanitizeAddress`) rather than a hand-rolled runner — a
// hand-rolled runner (e.g. shelling out to the standalone `bezelc`
// executable without ASan instrumentation wired the same way) never
// instruments generated code the same way and its green would be a SKIP.
// MEASURED: the cross-module CHOICE-element repro is a real
// heap-use-after-free pre-fix (identical signature). Post-fix the
// IMPORTED axis is now clean of BOTH UAF and leak (loop-amplified 100x,
// `imported_choice_element_array_clean_loop_amplified` row asserts the
// no-leak/no-UAF outcome directly rather than the prior 500 B leak
// floor — the original floor was the #4266/#4274-shaped residual this
// file's first version expected to remain, but `fix #4296`'s
// `inlineArrayStringDeclareThenAssignSite_` map closed that exact gap
// for the inline-array-of-Choice-element channel too; the row's
// assertion was updated per `fix-must-update-existing-tests.md`).
// Issues #4266 (cross-module Record declare-then-assign leak — closed by
// the same declare-then-assign mechanism) and #4274 (cross-module string-
// element declare-then-assign leak, the InlineArray/string-element twin
// — closed by #4296 directly) are the sibling tracking issues for the
// analogous shapes on the Record-only and InlineArray/string-only
// channels; both are addressed by the same declare-then-assign release-
// eligibility collector and were verified closed at the same time.
//
// See tests/codegen/record_choice_call_result_param_alias_uaf_4254_test.cpp
// for the top-level Record/Choice channel, tests/codegen/
// inline_array_string_call_result_param_alias_uaf_4265_test.cpp for the
// InlineArray/string-element sibling channel this file sits beside, and
// tests/codegen/imported_choice_call_result_param_alias_uaf_4260_test.cpp
// for the LibHandle/AppHandle harness this file's imported-axis row reuses
// verbatim.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::runAotChecked;

namespace {
// Strict oracle: compiled, ran, exited cleanly, no ASan/LSan finding. `ran`
// is false on a UAF (which routes through an ASan hard-abort, fork-isolated)
// exactly as it is on a leak — see #4254/#4265's identical pattern.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// PRIMARY PIN 1/2 — RECORD-typed element, the issue's own repro verbatim.
// FAIL-WITHOUT: AddressSanitizer heap-use-after-free (measured, see file
// header). PASS-WITH: exit 5 ("he"+"llo" = "hello", Length 5).
// ===========================================================================
BEZEL_TEST(inline_array_record_choice_element_call_result_param_alias_uaf_4280,
           narrow_scope_helper_returned_record_element_array_no_longer_uaf) {
    const char* prog =
        "module M; record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): [1]Str "
        "    return [new Str(s)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Str; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return outer[0].V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// PRIMARY PIN 2/2 — CHOICE-typed element, the issue's own repro verbatim.
// FAIL-WITHOUT: identical AddressSanitizer heap-use-after-free signature
// (measured, see file header). PASS-WITH: exit 5.
// ===========================================================================
BEZEL_TEST(inline_array_record_choice_element_call_result_param_alias_uaf_4280,
           narrow_scope_helper_returned_choice_element_array_no_longer_uaf) {
    const char* prog =
        "module M; choice Tag Named(V: string); end Tag; "
        "class Helper "
        "  public static method Wrap(s: string): [1]Tag "
        "    return [Tag.Named(s)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: [1]Tag; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    return (case outer[0] of Named(v) => v.Length); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// TYPE-KIND-BOUNDARY CONTROL — an OWNING inline array of a CLASS-typed
// element (`[1 owned]Widget`) is the axis this fix deliberately does NOT
// reach: `Widget`'s own explicit constructor field-store
// (`V := v;`) retains the borrowed `s` via `lowerStringFieldStore`, unlike
// a record's/choice's raw positional/payload store, so the #1430
// relaxation's soundness argument actually holds here — suppressing it
// would be pure over-suppression (an unforced extra leak). MEASURED clean
// (no ASan/LSan finding, exit 5) on BOTH `0c45cb6` (pre-this-fix) and this
// commit (post-this-fix): the guard's Record/Choice test never matches a
// Class declaration, so this row's outcome does not change across the fix.
// This is a FLAT scope deliberately (not the narrow do/end shape the PIN
// rows use above) — a `[N owned]T` local does not admit the
// declare-then-reassign shape those rows use (whole-value reassignment of
// an uninitialized move-only array is rejected by the compiler, measured
// separately, out of scope for this file), and a flat scope is sufficient
// here: this row's oracle is LEAK presence, which does not depend on
// read-before-free ordering the way the UAF pins above do.
// ===========================================================================
BEZEL_TEST(inline_array_record_choice_element_call_result_param_alias_uaf_4280,
           inline_array_owned_class_element_control_stays_clean_not_over_suppressed) {
    const char* prog =
        "module M; "
        "class Widget "
        "  public V: string; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Widget; "
        "class Helper "
        "  public static method Wrap(s: string): [1 owned]Widget "
        "    return [new Widget(s)]; "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer := Helper.Wrap(s); "
        "    return outer[0].V.Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// IMPORTED AXIS — the cross-module CHOICE-element repro, reusing #4260's
// LibHandle/AppHandle in-process two-module harness verbatim (do not
// hand-roll a runner — see file header). ASan-instrumented via
// `CodegenOptions.sanitizeAddress`.
//
// MEASURED (this send-back's own worktree): FAIL-WITHOUT (this file's own
// fix reverted) is a real heap-use-after-free, identical signature.
// PASS-WITH is clean of any UAF but leaks — loop-amplified 100x (a single
// helper call inside a `while` loop, mirroring
// inline_array_string_call_result_param_alias_uaf_4265_test.cpp's own
// `heap_array_string_factory_control` amplification pattern) and asserted
// as a FLOOR, never an exact byte count (§30).
// ===========================================================================
namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir4280() {
    return wtScratchDir("wt-4280");
}

struct TempFile4280 {
    std::string path;
    explicit TempFile4280(const std::string& p) : path(p) {}
    ~TempFile4280() { std::remove(path.c_str()); }
};

void writeSource4280(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle4280 {
    std::shared_ptr<TempFile4280> src;
    std::shared_ptr<TempFile4280> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle4280 buildLib4280(const fs::path& dir, const std::string& libSrc) {
    LibHandle4280 h;
    std::string stem = unique_stem("bezel_4280lib_");
    h.src = std::make_shared<TempFile4280>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile4280>((dir / (stem + "_lib.o")).string());
    writeSource4280(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle4280 {
    std::shared_ptr<TempFile4280> src;
    std::shared_ptr<TempFile4280> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle4280 buildApp4280(const fs::path& dir, const std::string& libObjPath,
                            const std::string& appSrc, bool sanitize) {
    AppHandle4280 h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_4280app_");
    h.src = std::make_shared<TempFile4280>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile4280>((dir / stem).string());
    writeSource4280(h.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult rb = driver.compileFile(h.src->path, h.exe->path,
                                                  deps, opts);
    h.ok = rb.success;
    h.exePath = h.exe->path;
    return h;
}

struct RunResult4280 { int code; std::string out; };

RunResult4280 runCapture4280(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// UAF-only headline: single call, no amplification, to keep the ASan
// signature isolated and legible.
const std::string kLibSrcUaf4280 =
    "module LibChoiceArr4280; "
    "public choice Tag4280 Named(V: string); end Tag4280; "
    "public class HelperChoiceArr4280 "
    "  public static method Wrap(s: string): [1]Tag4280 "
    "    return [Tag4280.Named(s)]; "
    "  end Wrap; "
    "end HelperChoiceArr4280; ";

const std::string kAppSrcUaf4280 =
    "module AppChoiceArr4280; "
    "import LibChoiceArr4280; "
    "public class CChoiceArr4280 "
    "  public static method Main(): int "
    "    var outer: [1]Tag4280; "
    "    do "
    "      var s := \"he\" + \"llo\"; "
    "      outer := LibChoiceArr4280.HelperChoiceArr4280.Wrap(s); "
    "    end; "
    "    return (case outer[0] of Named(v) => v.Length); "
    "  end Main; "
    "end CChoiceArr4280; ";

// Loop-amplified (100x) leak-floor variant of the same shape — §30: a
// one-shot LSan row can pass on both sides, so amplify before asserting.
const std::string kAppSrcLeak4280 =
    "module AppChoiceArrLeak4280; "
    "import LibChoiceArr4280; "
    "public class CChoiceArrLeak4280 "
    "  public static method Main(): int "
    "    var i := 0; "
    "    var total := 0; "
    "    while i < 100 do "
    "      var outer: [1]Tag4280; "
    "      do "
    "        var s := \"he\" + \"llo\" + i.ToString(); "
    "        outer := LibChoiceArr4280.HelperChoiceArr4280.Wrap(s); "
    "      end; "
    "      total += (case outer[0] of Named(v) => v.Length); "
    "      i += 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end CChoiceArrLeak4280; ";

}  // namespace

BEZEL_TEST(inline_array_record_choice_element_call_result_param_alias_uaf_4280,
           imported_choice_element_array_no_longer_uaf) {
    auto dir = wtDir4280();
    LibHandle4280 libH = buildLib4280(dir, kLibSrcUaf4280);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle4280 appH =
        buildApp4280(dir, libH.objPath, kAppSrcUaf4280, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult4280 rr = runCapture4280(appH.exePath);
    // Correctness oracle: "he"+"llo" = "hello", Length 5.
    BEZEL_EXPECT_EQ(rr.code, 5);
    // Soundness oracle — the load-bearing half.
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
}

// Clean — no UAF AND no leak — on the IMPORTED-CHOICE axis, loop-
// amplified 100x. Cycle-7 close-out: the #4266/#4274-shaped residual leak
// this row originally measured (and pinned as an assert-foundLeak floor
// per LANE_RULES §30) was CLOSED by `fix #4296`'s declare-then-assign
// `inlineArrayStringDeclareThenAssignSite_` map, which admits the
// `var outer: [1]Tag4280; outer := Helper.Wrap(s);` shape into
// `inlineArrayStringCallResultReleaseEligible_`. After #4296
// (commit 165489a, 2026-09-13) the +1 producer string argument's
// `inlineArrayStringFields` Disposal (wired in `lowerLocalDecl`) reclaims
// the buffer at `outer`'s own scope end — the SAME #4266/#4274-shaped
// gap that the file header named, fully closed for the inline-array-of-
// Choice-element channel.
//
// Per LANE_RULES `fix-must-update-existing-tests.md` ("When fix reverses
// a prior rejection, tests pinned to OLD behavior must update in same
// PR or gate reports as REAL regression") this row's expectation flips
// from "leak floor >=500 B" (the pre-#4296 fingerprint) to "no leak at
// all" (the post-#4296 reality). The UAF-clean half stays load-bearing —
// reverting #4280's `walkCallArgs` InlineArray guard (the [N]Record/
// [N]Choice half of `elementMayEmbedStringArgUnretained`) re-opens the
// heap-use-after-free on this exact 100-iteration program, so the no-UAF
// assertion below is the FAIL-WITHOUT pivot. The no-leak assertion is
// the FAIL-WITHOUT pivot for #4296's release-eligibility gap. Both are
// mechanically verified (see commit body).
//
// Reuses #4260's LibHandle/AppHandle two-module harness (`compileObject` /
// `compileFile` with `CodegenOptions.sanitizeAddress`) verbatim rather
// than hand-rolling a runner — a hand-rolled runner never instruments
// generated code the same way and its green would be a SKIP
// (LANE_RULES: "native ASan leg vacuous in lane worktrees"). 100x
// amplification mirrors `inline_array_string_call_result_param_alias_
// uaf_4265_test.cpp`'s `heap_array_string_factory_control` amplification
// pattern.
BEZEL_TEST(inline_array_record_choice_element_call_result_param_alias_uaf_4280,
           imported_choice_element_array_clean_loop_amplified) {
    auto dir = wtDir4280();
    LibHandle4280 libH = buildLib4280(dir, kLibSrcUaf4280);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle4280 appH =
        buildApp4280(dir, libH.objPath, kAppSrcLeak4280, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult4280 rr = runCapture4280(appH.exePath);
    // #4280 FAIL-WITHOUT pivot: reverting #4280's InlineArray guard
    // (the [N]Record/[N]Choice half of `elementMayEmbedStringArgUnretained`)
    // makes every one of the 100 calls a heap-use-after-free.
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
}

}  // namespace bezel::codegen
