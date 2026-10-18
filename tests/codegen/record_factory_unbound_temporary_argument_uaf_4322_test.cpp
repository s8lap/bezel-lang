// Codegen — issue #4322 / spec §9.7 ("Records and disposal") + §28.3
// ("Local variables, ownership, and borrows", the Producers table) +
// §28.9 ("Runtime-managed types"), docs revision v0.298.
//
// REGRESSION PIN. An UNBOUND temporary — a `+1` string producer (concat /
// interpolation) that appears DIRECTLY as a call argument, never bound to a
// local first — passed to a record factory that forwards it unmodified
// (`return new P(s);`) is a heap-use-after-free when the factory's result is
// consumed. This is the sibling of `fix #4254`/`fix #4260`/`fix #4265`/
// `fix #4280` (all closing the SAME `buildRecordValue`/inline-array
// raw-`CreateStore`-no-`emitStringRetain` alias for a NAMED-IDENTIFIER
// argument) for an argument that has NO binding at all, so
// `StringEscapeWalker::walkCallArgs`'s `isTargetIdent`-gated suppression
// never even considers it — a structurally separate code path from
// everything those fixes touch.
//
// ── ROOT CAUSE (read, not assumed) ────────────────────────────────────────
//
// `Emitter::lowerCallDispatch`'s `#429` "ARGUMENT-PASS category" release
// (LlvmBackend_Calls2.cpp, the `isPlusOneStringProducer(argExprs[ai])` ->
// unconditional `emitStringRelease(args[ai])` loop) runs at THE END OF THE
// CALL EXPRESSION for every `+1`-string-producer argument, regardless of the
// callee's return type. `Helper.Wrap(s: string): Str return new Str(s); end
// Wrap;`'s record-construction fast path (`buildRecordValue`,
// LlvmBackend_Exprs2.cpp) stores the argument into `Str.V` with a bare
// `CreateStore` — no `emitStringRetain`, exactly the fact `fix #4254`
// established for the NAMED-local channel. So `var outer :=
// Helper.Wrap("he"+"llo"+i.ToString());` freed the argument's own allocation
// immediately after `Wrap` returned, while `outer.V` still aliased it — a
// measured `AddressSanitizer: heap-use-after-free`.
//
// `record_factory_param_passthrough_admitted_4295_test.cpp`'s own header
// (§9.7/§28.3/§28.9) already states the soundness premise this bug violated:
// "admitting the alias as provably fresh is sound ONLY paired with an
// ALREADY-SHIPPED, call-WIDE caller-side suppression:
// `calleeReturnMayAliasArgUnretained` ... marks EVERY call whose resolved
// callee returns a Record/Choice as 'may alias an argument unretained',
// withholding ALL of that call's own argument-LOCALS from their own
// scope-end release". That suppression is `isTargetIdent`-gated
// (LlvmBackend_Setup.cpp ~line 23752) — it only ever examines a NAMED
// identifier argument. An unbound temporary is not a local at all, so
// nothing withheld its release, and `fix #4295`'s own admission (which
// makes `var outer := Helper.Wrap(s)` release-eligible whenever `s` is
// Wrap's own bare parameter) had no matching suppression on THIS shape —
// exactly the gap `fix #4295`'s header warns a change to
// `calleeReturnMayAliasArgUnretained`'s coverage would create if the two
// drifted apart.
//
// ── FIX SHAPE ──────────────────────────────────────────────────────────────
//
// `calleeReturnMayAliasArgUnretained`'s computation (Record/Choice return,
// or InlineArray-of-`string`/Record/Choice-element return — the
// #4254/#4265/#4280 channels) is factored out of `StringEscapeWalker::
// walkCallArgs` into a shared free function,
// `calleeReturnMayAliasStringArgUnretained` (LlvmBackend_impl.hpp, next to
// `isBuiltinDictionaryTypeRef` — same "one definition, two consumers" shape,
// for the same reason: the walker is a namespace-scope struct that cannot
// call a PRIVATE `Emitter` member, and `lowerCallDispatch` lives in a
// different translation unit). `lowerCallDispatch`'s unbound-temporary
// release loop now consults the SAME fact and skips the release, call-wide,
// exactly matching walkCallArgs' own contract (not per-argument-position —
// see `bare_parameter_and_unrelated_second_argument_no_longer_uaf_but_
// leaks_unrelated_arg` below for what that costs and why it is the
// pre-existing safe direction, not a new one).
//
// ── FAIL-WITHOUT / PASS-WITH (measured, this fix's own worktree; captured
// diff of the two `src/codegen/` files, `git checkout --` to strip the fix,
// rebuild `bezelc` explicitly [LANE_RULES §23 — the standalone-executable
// rows below shell out to it], re-measure, restore the diff, rebuild again)
// ───────────────────────────────────────────────────────────────────────────
//
//   - LOCAL axis, the issue's own repro verbatim (500 iters, loop-amplified
//     per §30 — matches the issue's own already-amplified measurement):
//     FAIL-WITHOUT is a real `AddressSanitizer: heap-use-after-free`
//     (READ of size 8, freed in `main`, previously allocated in `main`, the
//     same 32-byte `bz.string`-header region signature every sibling row in
//     this family measures). PASS-WITH: exit 50 (sum of
//     "he"+"llo"+i.ToString().Length for i in [0,500), mod 256 — AOT exit
//     status truncates to 8 bits), no ASan/LSan output.
//   - LOCAL axis, unrelated-second-argument shape (below): FAIL-WITHOUT is
//     the IDENTICAL heap-use-after-free signature (the embedded argument's
//     own release still fires unconditionally pre-fix, regardless of the
//     second, unrelated argument). PASS-WITH: no crash, but a real
//     `LeakSanitizer` report — 19890 B / 1000 allocations over 500
//     iterations — because the call-wide suppression also withholds the
//     UNRELATED argument's release; see that row's own comment for why this
//     is the pre-existing leak-over-UAF safe default, not new.
//   - IMPORTED axis, identical shape via `fix #4260`'s two-step `bezelc -c`
//     / `bezelc --import --sanitize=address` build (single call, no
//     amplification, isolated signature — mirrors `fix #4280`'s imported
//     UAF-headline row): FAIL-WITHOUT is a real heap-use-after-free,
//     identical signature. PASS-WITH: exit 5 ("he"+"llo".Length), no
//     ASan/LSan output.
//   - IMPORTED axis, loop-amplified (500 iters, same two-step build):
//     PASS-WITH is FULLY leak-clean too (exit 50, no ASan/LSan output at
//     all) — unlike the sibling `#4280`/`#4266`/`#4274` InlineArray channel,
//     which left a residual leak on the imported axis; this fix closes the
//     bare-record channel completely on both axes because `fix #4295`'s own
//     release-eligibility mechanism (`collectRecordCallResultReleaseEligible`)
//     already covers `var outer := <call>;` on both module axes — this fix
//     only had to stop double-claiming the argument's own +1.
//
// So 3 of 4 UAF/leak rows fail without the fix and pass with it (named
// above: local headline, local unrelated-argument, imported headline). The
// imported loop-amplified row is a POST-fix-only leak-magnitude check (no
// meaningful pre-fix state to compare — the program never completes a
// single iteration pre-fix, it UAFs on iteration 0).
//
// ── BOUNDARY CONTROL — a CLASS-returning callee is UNCHANGED ─────────────
//
// A class's own explicit field-store constructor (`V := v;`) retains the
// borrowed string via `lowerStringFieldStore`, unlike a record's/choice's
// raw positional-field store — so the #429 release is ALREADY safe there
// (rc: mint 1, retain 2, release 1, class disposal releases the last 1) and
// must NOT be suppressed. `calleeReturnMayAliasStringArgUnretained`'s
// `isRecordOrChoiceDeclared` test structurally excludes `TypeDeclKind::
// Class` (only `Record`/`Choice` match), so this row is unaffected BY
// CONSTRUCTION — verified empirically too: MEASURED identical, exit 50, no
// ASan/LSan output, on BOTH the pre-fix parent build and this fix's build
// (500 iterations, `using var` per-iteration disposal).
//
// See tests/codegen/record_choice_call_result_param_alias_uaf_4254_test.cpp
// for the top-level Record/Choice named-local channel,
// record_factory_param_passthrough_admitted_4295_test.cpp for the
// release-eligibility half this fix's soundness argument depends on not
// having drifted, and imported_choice_call_result_param_alias_uaf_4260_test
// .cpp for the LibHandle/AppHandle harness this file's imported-axis rows
// reuse verbatim (do not hand-roll a runner — a hand-rolled runner never
// instruments generated code, so its green is a SKIP).

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
// exactly as it is on a leak — see #4254/#4265/#4280's identical pattern.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    return r.ran ? r.exitCode : -999;
}

// leaksAtLeast — a measured, margined leak-magnitude floor oracle (§30:
// NEVER pin an exact byte count). Pattern copied from
// record_factory_param_passthrough_admitted_4295_test.cpp's own
// identically-named helper.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    if (r.ran) return false;  // no sanitizer finding at all — wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;
}
}  // namespace

// ===========================================================================
// PRIMARY PIN — LOCAL axis, the issue's own repro verbatim, loop-amplified
// 500x (§30). FAIL-WITHOUT: AddressSanitizer heap-use-after-free (measured,
// see file header). PASS-WITH: exit 50.
// ===========================================================================
BEZEL_TEST(record_factory_unbound_temporary_argument_uaf_4322,
           local_unbound_temporary_argument_no_longer_uaf) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var outer := Helper.Wrap(\"he\" + \"llo\" + i.ToString()); "
        "      total := total + outer.V.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// LOCAL axis — a SECOND, unrelated `+1`-producer argument the callee does
// NOT embed anywhere. FAIL-WITHOUT: the identical heap-use-after-free (the
// EMBEDDED argument `s` still gets its unconditional pre-fix release, which
// is the actual bug regardless of the second argument). PASS-WITH: no
// crash, but the call-wide (not per-argument) suppression ALSO withholds
// `unused`'s own release, so `unused`'s allocation leaks — the pre-existing
// leak-over-UAF safe default `fix #4280`'s own commit documents ("a possible
// extra leak, never new unsoundness"), matching how
// `record_factory_param_passthrough_admitted_4295_test.cpp`'s
// `double_embed_same_parameter_stays_rejected_no_uaf` control pins the same
// principle for the NAMED-local channel. Floor 15000 comfortably below the
// measured 19890 B / 1000 allocations (§30: floor, not an exact count).
// ===========================================================================
BEZEL_TEST(record_factory_unbound_temporary_argument_uaf_4322,
           unrelated_second_argument_no_longer_uaf_but_leaks_call_wide) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Helper "
        "  public static method Wrap(s: string, unused: string): Str "
        "    return new Str(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      var outer := Helper.Wrap(\"he\" + \"llo\" + i.ToString(), "
        "                                \"junk\" + i.ToString()); "
        "      total := total + outer.V.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 15000));
}

// ===========================================================================
// BOUNDARY CONTROL — a CLASS-returning callee (an explicit field-store
// constructor, `lowerStringFieldStore` retains) is UNAFFECTED. Not a
// FAIL-WITHOUT row: MEASURED IDENTICAL (exit 50, no ASan/LSan output) on
// both the pre-fix parent build and this fix — see file header.
// ===========================================================================
BEZEL_TEST(record_factory_unbound_temporary_argument_uaf_4322,
           class_returning_factory_control_stays_clean_unaffected) {
    const char* prog =
        "module M; "
        "class Widget "
        "  public V: string; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Widget; "
        "class Helper "
        "  public static method Wrap(s: string): owned Widget "
        "    return new Widget(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    var i: int := 0; "
        "    while i < 500 do "
        "      using var outer := Helper.Wrap(\"he\" + \"llo\" + i.ToString()); "
        "      total := total + outer.V.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return total mod 256; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 50);
}

// ===========================================================================
// IMPORTED AXIS — reuses `fix #4260`'s LibHandle/AppHandle in-process
// two-module harness verbatim (do not hand-roll a runner — see file
// header). ASan-instrumented via `CodegenOptions.sanitizeAddress`.
// ===========================================================================
namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir4322() {
    return wtScratchDir("wt-4322");
}

struct TempFile4322 {
    std::string path;
    explicit TempFile4322(const std::string& p) : path(p) {}
    ~TempFile4322() { std::remove(path.c_str()); }
};

void writeSource4322(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle4322 {
    std::shared_ptr<TempFile4322> src;
    std::shared_ptr<TempFile4322> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle4322 buildLib4322(const fs::path& dir, const std::string& libSrc) {
    LibHandle4322 h;
    std::string stem = unique_stem("bezel_4322lib_");
    h.src = std::make_shared<TempFile4322>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile4322>((dir / (stem + "_lib.o")).string());
    writeSource4322(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle4322 {
    std::shared_ptr<TempFile4322> src;
    std::shared_ptr<TempFile4322> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle4322 buildApp4322(const fs::path& dir, const std::string& libObjPath,
                            const std::string& appSrc, bool sanitize) {
    AppHandle4322 h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_4322app_");
    h.src = std::make_shared<TempFile4322>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile4322>((dir / stem).string());
    writeSource4322(h.src->path, appSrc);
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

struct RunResult4322 { int code; std::string out; };

RunResult4322 runCapture4322(const std::string& exe) {
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
// signature isolated and legible (mirrors #4280's imported UAF-headline
// row).
const std::string kLibSrcUaf4322 =
    "module Lib4322x; "
    "public record Str4322x(V: string); "
    "public class Helper4322x "
    "  public static method Wrap(s: string): Str4322x "
    "    return new Str4322x(s); "
    "  end Wrap; "
    "end Helper4322x; ";

const std::string kAppSrcUaf4322 =
    "module App4322x; "
    "import Lib4322x; "
    "public class C4322x "
    "  public static method Main(): int "
    "    var outer := Lib4322x.Helper4322x.Wrap(\"he\" + \"llo\"); "
    "    return outer.V.Length; "
    "  end Main; "
    "end C4322x; ";

// Loop-amplified (500x) variant of the same shape (§30 — a one-shot LSan/ASan
// row can pass on both sides).
const std::string kAppSrcLoop4322 =
    "module App4322xLoop; "
    "import Lib4322x; "
    "public class C4322xLoop "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var outer := Lib4322x.Helper4322x.Wrap(\"he\" + \"llo\" + i.ToString()); "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total mod 256; "
    "  end Main; "
    "end C4322xLoop; ";

}  // namespace

BEZEL_TEST(record_factory_unbound_temporary_argument_uaf_4322,
           imported_unbound_temporary_argument_no_longer_uaf) {
    auto dir = wtDir4322();
    LibHandle4322 libH = buildLib4322(dir, kLibSrcUaf4322);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle4322 appH =
        buildApp4322(dir, libH.objPath, kAppSrcUaf4322, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult4322 rr = runCapture4322(appH.exePath);
    // Correctness oracle: "he"+"llo" = "hello", Length 5.
    BEZEL_EXPECT_EQ(rr.code, 5);
    // Soundness oracle — the load-bearing half.
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
}

// Loop-amplified imported axis: MEASURED fully leak-clean post-fix (unlike
// the sibling #4280/#4266/#4274 InlineArray channel's residual leak — see
// file header) — asserted here as "no sanitizer report at all", the
// strongest claim this harness can make, not merely "no UAF".
BEZEL_TEST(record_factory_unbound_temporary_argument_uaf_4322,
           imported_unbound_temporary_argument_loop_amplified_fully_clean) {
    auto dir = wtDir4322();
    LibHandle4322 libH = buildLib4322(dir, kLibSrcUaf4322);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle4322 appH =
        buildApp4322(dir, libH.objPath, kAppSrcLoop4322, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult4322 rr = runCapture4322(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 50);
    BEZEL_EXPECT_TRUE(rr.out.find("AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("heap-use-after-free") == std::string::npos);
}

}  // namespace bezel::codegen
