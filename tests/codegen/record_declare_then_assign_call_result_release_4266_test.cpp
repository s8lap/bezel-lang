// Codegen — issue #4266 / spec §9.7 ("Records and disposal") + §28.9
// ("Runtime-managed types") + §2.8.3 rule 4 ("Cross-module type/field/method
// surface"), docs revision v0.298.
//
// REGRESSION PIN. A record-returning factory's result bound to a local via
// "declare, then assign" — `var outer: P; ...; outer := Mk(...);` — rather
// than `var outer := Mk(...);` must NOT leak the record's `string` field(s),
// on BOTH the same-module and cross-module (imported) axes.
//
// ROOT CAUSE (measured, this tree): `Emitter::collectRecordCallResultRelease
// Eligible` (LlvmBackend_Setup.cpp) scanned ONLY a `var`/`const` LocalDecl's
// OWN `initializer` field (via `LocalDeclInitView`) for a record-returning
// CallExpr/NewExpr — never a later plain `:=` AssignmentStmt whose target
// resolves to an earlier, initializer-less LocalDecl. So `outer := Mk(...);`
// was never a release-eligible candidate, on EITHER module axis, and the
// callee-minted +1 on each string field leaked at `outer`'s scope end.
//
// ⚠ CORRECTS THE ISSUE'S OWN "unexplained asymmetry" CLAIM. #4266 measured
// the cross-module leg leaking (38 B, single-shot) and the same-module leg
// CLEAN (single-shot), and asked what second mechanism disposes the local
// axis. There is no second mechanism — LOOP-AMPLIFYING (500x, per LANE
// policy: a one-shot LSan row can pass on BOTH sides) shows the SAME-MODULE
// axis ALSO leaks identically (measured: 20848 B / 998 allocations over 500
// iterations, pre-fix, for the shape this file pins) — the single-shot
// "clean" reading was LeakSanitizer's conservative-stack-scan nondeterminism
// (§30), not a real disposal path. Both axes were broken; both are fixed
// here by the SAME change (collector + escape-walker), in ONE commit.
//
// ── FIX SHAPE (§9.7 / §28.9) ────────────────────────────────────────────
//
// `collectRecordCallResultReleaseEligible` now ALSO scans, in the SAME
// single body walk, for a plain `var outer: P;` (no initializer) whose
// declared type is a record with string fields, and tracks every `:=`
// AssignmentStmt whose target resolves to that declaration. A candidate is
// admitted ONLY when it has EXACTLY ONE such assignment site in the WHOLE
// function body, and that site's RHS independently satisfies the identical
// freshness proof a `var p := <call>;` initializer already needs
// (`isProvablyFreshRecordFactoryMethod` / its imported twin /
// `isSynthesizedRecordDeserializeCall` / `isFreshRecordConstructionOf
// ExactType` — the EXISTING authorities, not re-derived, per lane
// discipline §6/§25). A second assignment to the same target permanently
// disqualifies it (falls back to the pre-existing safe-leak behaviour —
// see `multi_assignment_site_stays_safe_no_uaf` below).
//
// `StringEscapeWalker` gained one opt-in field, `recordDeclareThenAssignSite`
// — the identity of the ONE AssignmentStmt node the collector already
// vetted — so that ONE store is treated as the candidate's non-escaping
// "initializer" (exactly as a LocalDecl's own `initializer` already is,
// implicitly, by never being visited as a generic statement at all); any
// OTHER `:=` to the same target still falls to the pre-existing conservative
// `escaped = true` in `recordFieldReleaseMode`.
//
// `lowerLocalDecl`'s existing recordStringFields registration required `v`
// (the LOWERED INITIALIZER's value) to be non-null — always true for the
// pre-existing `var p := <call>;` shape, but a no-init `var outer: P;` has
// no initializer to lower at all (`v` is unconditionally null there). The
// gate is widened to `v || recordDeclareThenAssignSite_.count(declNode)` —
// admitting ONLY declNodes the collector already vetted, no broader than
// `recordCallResultReleaseEligible_` itself already requires two lines down.
//
// ── SCOPE — WHAT THIS FIX DOES NOT REACH (separate, deeper, PRE-EXISTING
// gap; filed, not fixed here) ──────────────────────────────────────────
//
// `isFreshRecordConstructionOfExactType`'s `string`-field check requires
// the field's constructor ARGUMENT be a literal or `isPlusOneStringProducer`
// (a `+`/interpolation/etc.) — a BARE PARAMETER READ (`return new P(s);`
// where `s` is the callee's OWN string parameter) is neither, so it is
// REJECTED. This is a SEPARATE, pre-existing gap: it ALSO rejects the
// already-shipped `var p := Mk(s)` LocalDecl-with-init shape (measured
// below, both module axes) — independent of this fix's declare-then-assign
// scan, and independent of which module the factory lives in. #4266's own
// filed repro happens to use exactly this bare-parameter shape, so this fix
// does NOT make #4266's own program leak-clean — see
// `bare_parameter_passthrough_control_still_leaks_separate_gap` below and
// the commit body for the filed follow-up issue.
//
// MEASURED (isolated worktree, this tree — commit FIRST, then `git checkout
// --` to strip the fix, rebuild `bezelc` explicitly per LANE_RULES §23,
// re-run under a direct `--sanitize=address` two-step `bezelc -c` /
// `bezelc --import` build (and a single-module build for the local leg),
// `git apply` to restore, rebuild again):
//   - same-module declare-then-assign producer factory (500 iters): FAIL-
//     WITHOUT is a real LeakSanitizer report, 20848 B / 998 allocations
//     (`bz$M.Helper$Wrap` as the allocation site). PASS-WITH: exit 38
//     (`sum("hello-N".Length, N in [0,500)) mod 256`), no ASan/LSan output.
//   - imported (cross-module) declare-then-assign producer factory (500
//     iters, two-step `bezelc -c` / `bezelc --import --sanitize=address`
//     build): FAIL-WITHOUT is the IDENTICAL 20848 B / 998 allocations
//     signature. PASS-WITH: exit 38, no ASan/LSan output.
// So 2 of 2 leak rows fail without the fix and pass with it (named above).
// The bare-parameter-passthrough control and the multi-assignment-site
// safety control are NOT FAIL-WITHOUT rows — both are UNAFFECTED by this
// fix by design (see each row's own comment) and are included to pin the
// fix's boundary and its soundness restriction, not to re-measure the fix
// itself.
//
// See tests/README.md for the `runAot`/`runAotChecked` harness, and
// imported_record_string_field_call_scope_end_release_3621_test.cpp for the
// cross-module build helper pattern this file follows (its own `var p :=
// <call>;` shape is UNAFFECTED by this fix — re-swept green as part of this
// fix's own verification, not re-pinned here to avoid a duplicate test).

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
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md). The library object MUST outlive the app build.
static const std::filesystem::path& wtDir4266() {
    return wtScratchDir("wt-4266");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const fs::path& dir, const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_4266lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

AppHandle buildApp(const fs::path& dir, const std::string& libObjPath,
                   const std::string& appSrc, bool sanitize) {
    AppHandle h;
    if (libObjPath.empty()) return h;
    std::string stem = unique_stem("bezel_4266app_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, appSrc);
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

struct LocalHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
    bool ok = false;
};

LocalHandle buildLocalApp(const fs::path& dir, const std::string& src,
                          bool sanitize) {
    LocalHandle h;
    std::string stem = unique_stem("bezel_4266local_");
    h.src = std::make_shared<TempFile>((dir / (stem + ".bzl")).string());
    h.exe = std::make_shared<TempFile>((dir / stem).string());
    writeSource(h.src->path, src);
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = sanitize;
    bezel::CompileResult r =
        driver.compileFile(h.src->path, h.exe->path, {}, opts);
    h.ok = r.success;
    h.exePath = h.exe->path;
    return h;
}

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Producer-shaped factory (matches #3621's own admitted shape: the
// string field's ctor argument is a `+` producer expression, not a bare
// parameter passthrough — see the file header's SCOPE section for why that
// distinction matters). Cross-module: imported record, imported factory.
const std::string kLibSrc4266 =
    "module Lib4266; "
    "public record Str4266(V: string); "
    "public class Helper4266 "
    "  public static method Wrap(n: int): Str4266 => "
    "    new Str4266(\"hello-\" + n.ToString()); "
    "end Helper4266; ";

// DECLARE-THEN-ASSIGN consumer: `var outer: Lib4266.Str4266;` has NO
// initializer; its value arrives via a later `outer := ...;` inside a
// narrower `do ... end;` block. 500 iterations (loop-amplified per LANE
// policy — a one-shot row can pass vacuously on both sides).
const std::string kAppSrc4266 =
    "module App4266; "
    "import Lib4266; "
    "public class C4266 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var outer: Lib4266.Str4266; "
    "      do "
    "        outer := Lib4266.Helper4266.Wrap(i); "
    "      end; "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4266; ";

// The character-identical SAME-MODULE twin, same declare-then-assign shape.
const std::string kLocalSrc4266 =
    "module Local4266; "
    "public record Str4266(V: string); "
    "public class Helper4266 "
    "  public static method Wrap(n: int): Str4266 => "
    "    new Str4266(\"hello-\" + n.ToString()); "
    "end Helper4266; "
    "public class C4266 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var outer: Str4266; "
    "      do "
    "        outer := Helper4266.Wrap(i); "
    "      end; "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4266; ";

// BOUNDARY CONTROL — the bare-parameter-passthrough shape (#4266's own
// filed repro, verbatim in structure): `Wrap(s: string): P return new
// P(s); end Wrap;`. NOT admitted by `isFreshRecordConstructionOfExactType`
// (a bare parameter read is neither a literal nor a `+1` producer) — a
// separate, pre-existing gap this fix does not reach (see file header).
// Still leaks post-fix; loop-amplified (500 iters) and measured with a
// FLOOR (§30 — LeakSanitizer's conservative stack scan is nondeterministic,
// never assert an exact byte count). Measured this tree, post-fix: 20390 B
// / 1000 allocations.
const std::string kBareParamSrc4266 =
    "module BareParam4266; "
    "public record Str4266bp(V: string); "
    "public class Helper4266bp "
    "  public static method Wrap(s: string): Str4266bp "
    "    return new Str4266bp(s); "
    "  end Wrap; "
    "end Helper4266bp; "
    "public class C4266bp "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var outer: Str4266bp; "
    "      do "
    "        var s := \"he\" + \"llo\" + i.ToString(); "
    "        outer := Helper4266bp.Wrap(s); "
    "      end; "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4266bp; ";

// SAFETY CONTROL — a target assigned TWICE (unconditionally, in sequence)
// disqualifies admission (`assignCount == 1` is required) — BOTH values
// leak (the pre-existing safe-leak default), never a double-free/UAF.
// Measured this tree, post-fix: 42390 B / 2000 allocations (both `Wrap`
// call sites attributed) — comfortably explained as "leaks everything,
// admits nothing", not a partial/unsound admission.
const std::string kMultiAssignSrc4266 =
    "module MultiAssign4266; "
    "public record Str4266ma(V: string); "
    "public class Helper4266ma "
    "  public static method Wrap(n: int): Str4266ma => "
    "    new Str4266ma(\"hello-\" + n.ToString()); "
    "end Helper4266ma; "
    "public class C4266ma "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var outer: Str4266ma; "
    "      outer := Helper4266ma.Wrap(i); "
    "      outer := Helper4266ma.Wrap(i + 1000); "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4266ma; ";

// leaksAtLeast — a measured, margined leak-magnitude floor oracle (§30:
// NEVER pin an exact byte count). Pattern copied from
// nested_record_string_field_leak_4237_test.cpp / inline_array_string_call
// _result_param_alias_uaf_4265_test.cpp's own identically-named helpers.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = bezel::test::runAotChecked(program);
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

// ── HEADLINE — declare-then-assign, IMPORTED axis, ASan, no leak ─────────
BEZEL_TEST(record_declare_then_assign_call_result_release_4266,
           imported_declare_then_assign_producer_no_leak) {
    auto dir = wtDir4266();
    LibHandle libH = buildLib(dir, kLibSrc4266);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc4266, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

// ── REGRESSION PIN — declare-then-assign, SAME-MODULE axis, ASan, no leak
//
// Corrects the issue's own "the local axis is naturally clean" premise: it
// is NOT (measured 20848 B / 998 allocations pre-fix over 500 iterations,
// identical in shape to the imported leg) — the single-shot reading that
// premise relied on was a LeakSanitizer false negative (§30).
// ───────────────────────────────────────────────────────────────────────
BEZEL_TEST(record_declare_then_assign_call_result_release_4266,
           local_declare_then_assign_producer_no_leak) {
    auto dir = wtDir4266();
    LocalHandle h = buildLocalApp(dir, kLocalSrc4266, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(h.ok);
    if (!h.ok) return;
    RunResult rr = runCapture(h.exePath);
    BEZEL_EXPECT_TRUE(rr.code >= 0);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

// ── BOUNDARY CONTROL — bare-parameter-passthrough factory still leaks ────
//
// NOT a FAIL-WITHOUT/PASS-WITH row: unaffected by this fix in either
// direction (a separate, pre-existing gap — see file header SCOPE
// section). Pins that this fix does not silently paper over #4266's own
// exact repro shape; the follow-up issue for THIS gap is cited in the
// commit body.
// ───────────────────────────────────────────────────────────────────────
BEZEL_TEST(record_declare_then_assign_call_result_release_4266,
           bare_parameter_passthrough_control_still_leaks_separate_gap) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(kBareParamSrc4266, 15000));
}

// ── SAFETY CONTROL — a multiply-assigned target stays safe (no UAF) ──────
BEZEL_TEST(record_declare_then_assign_call_result_release_4266,
           multi_assignment_site_stays_safe_no_uaf) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(kMultiAssignSrc4266, 30000));
}

}  // namespace bezel::codegen
