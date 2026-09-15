// Codegen — issue #4295 / spec §9.7 ("Records and disposal") + §28.3
// ("Local variables, ownership, and borrows", the Producers table) +
// §28.9 ("Runtime-managed types"), docs revision v0.298.
//
// REGRESSION PIN. `Emitter::isFreshRecordConstructionOfExactType`'s
// `string`-field check (LlvmBackend_Setup.cpp) rejected a bare-parameter-
// passthrough constructor argument — `return new P(s);` where `s` is the
// enclosing factory METHOD's own `string` parameter — as not provably
// fresh, so `isProvablyFreshRecordFactoryMethod` never admitted a factory
// of exactly this shape. That blocked the caller's scope-end
// `recordStringFields` release for every consumer of such a factory —
// `var p := Wrap(s);` (#1747/#2448/#3621) and the "declare, then assign"
// form (#4266) alike — leaking the callee-minted `string` on every call,
// on BOTH module axes.
//
// This is precisely the shape #4266's OWN filed repro used; `narrow #4266`
// closed the SEPARATE "declare, then assign" scan gap but left this issue's
// own boundary-control row (`bare_parameter_passthrough_control_still_
// leaks_separate_gap` in record_declare_then_assign_call_result_release_
// 4266_test.cpp) still leaking — see this fix's changes to that file.
//
// ── DOES `new P(s)` COPY OR ALIAS `s`? (read, not assumed) ───────────────
//
// The record-construction FAST PATH (LlvmBackend_Exprs2.cpp, the
// positional-field store loop, ~line 9899: `builder_.CreateStore(v,
// builder_.CreateStructGEP(st, slot, idxIt->second));`) stores the lowered
// argument value with a BARE store — no `emitStringRetain` call anywhere in
// that loop. So `new P(s)` ALIASES `s`'s heap string; it does not
// independently retain it. Per spec, this is exactly what a plain
// parameter's Producers-table row promises and no more:
//
//   "Parameter `p` (no modifier) | Borrowed (source: caller)"
//     (§28.3, the Producers table)
//
// and record storage of it stays a borrow, never taking ownership:
//
//   "Reference-typed fields in records are always borrows; the record does
//   not own what they point to." (§9.7)
//
// So admitting the alias as "provably fresh" is sound ONLY paired with an
// ALREADY-SHIPPED, call-WIDE caller-side suppression:
// `calleeReturnMayAliasArgUnretained` (`StringEscapeWalker::walkCallArgs`,
// LlvmBackend_Setup.cpp ~line 23561, landed across #4254/#4260/#4265/#4280)
// marks EVERY call whose resolved callee returns a Record/Choice as "may
// alias an argument unretained", withholding ALL of that call's own
// argument-locals from their own scope-end release — regardless of the
// call's syntactic position (assignment RHS, nested argument, etc.), since
// it is a property of the CALL NODE, computed once from the callee's
// return type, not a property of any one caller shape. So the single +1 an
// argument's own producer minted is NEVER independently released at a call
// site whose callee returns a Record/Choice; this fix makes the RETURNED
// record's own field the SOLE place that claims it — exactly one release
// for exactly one +1, at the record-local's own scope-end
// `recordStringFields` disposal (#1747/#4266).
//
// ── WIDEN vs REFUTE — and why the fix widens `isProvablyFreshRecordFactory
// Method`'s callee-body walk, NOT the shared `isFreshRecordConstructionOf
// ExactType` predicate itself ────────────────────────────────────────────
//
// `isFreshRecordConstructionOfExactType` is a SHARED eligibility authority
// (LANE_RULES §25): `fix #4253`'s class-field release gate
// (LlvmBackend_Disposal.cpp ~1746) and `boxValueNeedsStringRetain`'s own
// NewExpr arm (LlvmBackend_Disposal.cpp ~5284) both call it DIRECTLY on a
// `new P(...)` construction site that is NOT inside a factory-method body —
// a class field initializer, or an arbitrary boxed-value expression. At
// EITHER of those call sites a bare identifier could resolve to a STATIC
// field, another LOCAL, or (in principle) some OTHER method's parameter —
// none of which the call-wide `calleeReturnMayAliasArgUnretained`
// suppression above has any relationship to, because there is no CALL at
// all at a class-field-initializer or box-value site: the widening's
// entire soundness argument is anchored on a CALL boundary that does not
// exist there. Widening the SHARED predicate unconditionally would
// therefore be sound at the factory-body call site and UNSOUND at these
// others.
//
// The fix instead adds `enclosingFactory` — a new, DEFAULTED (nullptr)
// third parameter to `isFreshRecordConstructionOfExactType` — and passes it
// ONLY from `isProvablyFreshRecordFactoryMethod`'s own THREE callee-body-
// walk call sites (the expression-bodied, one-statement, and two-statement
// admitted shapes). Every OTHER call site (the #4237 nested-record
// recursion inside the SAME function, #4253's class-field gate,
// `boxValueNeedsStringRetain`'s NewExpr arm, and the #2866 direct-NewExpr
// admission in `collectRecordCallResultReleaseEligible`) omits the third
// argument and is BEHAVIORALLY UNCHANGED — see the accept-side sweep below
// and the fix's own commit body for the measured filter list.
//
// The admission itself is bounded: a bare-identifier argument is admitted
// ONLY when (a) it resolves to one of `enclosingFactory`'s OWN parameters
// (pointer identity against `ms->params`, never by name), (b) that
// parameter's modifier is EMPTY (a plain, unmarked "Borrowed (source:
// caller)" parameter per the Producers-table row quoted above — `ref`/
// `out`/`owned` have different ownership contracts and are excluded), and
// (c) that parameter occurs EXACTLY ONCE across the construction's own
// positional argument list. (b) and (c) are both exercised by dedicated
// controls below.
//
// ── FAIL-WITHOUT / PASS-WITH (measured, this fix's own worktree; commit
// FIRST, `git diff` captured, `git checkout --` the two `src/codegen/`
// files to strip the fix, rebuild `bezelc` explicitly [LANE_RULES §23 — the
// standalone-executable rows below shell out to it], re-measure, `git
// apply` the captured diff back, rebuild again) ──────────────────────────
//
//   - LOCAL axis, #4295's own repro (500 iters): FAIL-WITHOUT is a real
//     LeakSanitizer report, 20390 B / 1000 allocations (`bz$M.Helper$Wrap`
//     as the allocation site — the same magnitude the issue's own repro
//     measured, §30: exact counts are nondeterministic run to run).
//     PASS-WITH: exit 50 (sum of "he"+"llo"+i.ToString().Length for i in
//     [0,500), mod 256 — AOT exit status truncates to 8 bits), no
//     ASan/LSan output.
//   - IMPORTED axis, identical shape via #4260's LibHandle/AppHandle
//     two-module harness (500 iters, measured via a manual two-step
//     `bezelc -c` / `bezelc --import --sanitize=address` build of the
//     same program): FAIL-WITHOUT is a real LeakSanitizer report, 20349 B
//     / 998 allocations — the same order of magnitude as the local axis,
//     not byte-identical (§30). PASS-WITH: exit 50, no ASan/LSan output.
//   - record_declare_then_assign_call_result_release_4266_test.cpp's
//     `bare_parameter_passthrough_control_still_leaks_separate_gap`
//     (renamed here to `..._now_admitted_no_leak`, same source, combining
//     #4266's OWN "declare, then assign" shape with this fix's admission):
//     FAIL-WITHOUT is `leaksAtLeast(kBareParamSrc4266, 15000)` (that file's
//     existing floor) TRUE; PASS-WITH it is FALSE (clean).
//
// So 3 of 3 leak rows fail without the fix and pass with it (named above).
// The double-embed and ref-modifier controls below are NOT FAIL-WITHOUT
// rows — both are UNAFFECTED by this fix (they were rejected before, and
// stay rejected after) and are included to pin the admission's soundness
// boundary, not to re-measure the fix itself: MEASURED, this tree, POST-
// FIX: both leak 20390 B / 1000 allocations (§30: floor, not an exact
// count — LeakSanitizer's conservative stack scan is nondeterministic).
//
// See tests/README.md for the `runAot`/`runAotChecked` harness, and
// imported_choice_call_result_param_alias_uaf_4260_test.cpp for the
// LibHandle/AppHandle two-module build harness this file's imported-axis
// test reuses verbatim (not hand-rolled).

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

using bezel::test::runAotChecked;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md), mirroring wtDir4260()/wtDir4266() in the sibling files
// this harness is copied from.
static const std::filesystem::path& wtDir4295() {
    return wtScratchDir("wt-4295");
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
    std::string stem = unique_stem("bezel_4295lib_");
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
    std::string stem = unique_stem("bezel_4295app_");
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

// leaksAtLeast — a measured, margined leak-magnitude floor oracle (§30:
// NEVER pin an exact byte count). Pattern copied from
// record_declare_then_assign_call_result_release_4266_test.cpp's own
// identically-named helper.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
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

// ── HEADLINE 1/2 — LOCAL axis. #4295's own repro, verbatim. 500 iterations
// (loop-amplified per §30 — a one-shot LSan row can pass on BOTH sides).
const std::string kLocalSrc4295 =
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
    "      var s := \"he\" + \"llo\" + i.ToString(); "
    "      var outer := Helper.Wrap(s); "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C; ";

// ── HEADLINE 2/2 — IMPORTED axis, character-identical shape split across a
// library/application module pair.
const std::string kLibSrc4295 =
    "module Lib4295; "
    "public record Str4295(V: string); "
    "public class Helper4295 "
    "  public static method Wrap(s: string): Str4295 "
    "    return new Str4295(s); "
    "  end Wrap; "
    "end Helper4295; ";

const std::string kAppSrc4295 =
    "module App4295; "
    "import Lib4295; "
    "public class C4295 "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var s := \"he\" + \"llo\" + i.ToString(); "
    "      var outer := Lib4295.Helper4295.Wrap(s); "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C4295; ";

// ── CONTROL — the SAME parameter embedded into TWO string fields
// (`new TwoStr(s, s)`) must NOT be admitted: the `factoryParamArgCounts
// [ps] == 1` guard exists exactly to reject this shape. If it were wrongly
// admitted, the record's own scope-end `recordStringFields` release walk
// would call `bz_string_release` on the SAME pointer for BOTH fields —
// only ONE +1 was ever minted upstream, so the second release would be a
// double-free. MEASURED, this tree, POST-fix: still leaks, 20390 B / 1000
// allocations over 500 iterations — no crash, no double-free/UAF signature
// (rejection, not admission, exactly as the guard is designed to produce).
const std::string kDoubleEmbedSrc4295 =
    "module M; "
    "record TwoStr(A: string, B: string); "
    "class Helper "
    "  public static method Wrap(s: string): TwoStr "
    "    return new TwoStr(s, s); "
    "  end Wrap; "
    "end Helper; "
    "class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var s := \"he\" + \"llo\" + i.ToString(); "
    "      var outer := Helper.Wrap(s); "
    "      total := total + outer.A.Length + outer.B.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C; ";

// ── CONTROL — a `ref string` parameter must NOT be admitted: the fix's
// `ps->modifier.empty()` guard restricts admission to a PLAIN, unmarked
// parameter — the exact shape the §28.3 Producers-table row "Parameter `p`
// (no modifier) | Borrowed (source: caller)" describes and this fix's
// soundness argument is built on. A `ref string` parameter has a different
// ownership/aliasing contract this fix does not reason about, so it stays
// conservatively excluded. MEASURED, this tree, POST-fix: still leaks,
// 20390 B / 1000 allocations over 500 iterations — no crash.
const std::string kRefParamSrc4295 =
    "module M; "
    "record Str(V: string); "
    "class Helper "
    "  public static method Wrap(ref s: string): Str "
    "    return new Str(s); "
    "  end Wrap; "
    "end Helper; "
    "class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      var s := \"he\" + \"llo\" + i.ToString(); "
    "      var outer := Helper.Wrap(ref s); "
    "      total := total + outer.V.Length; "
    "      i := i + 1; "
    "    end while; "
    "    return total; "
    "  end Main; "
    "end C; ";

}  // namespace

// ── HEADLINE — LOCAL axis, no leak, correct value ─────────────────────────
BEZEL_TEST(record_factory_param_passthrough_admitted_4295,
           local_bare_parameter_passthrough_no_longer_leaks) {
    auto r = runAotChecked(kLocalSrc4295);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 50);
}

// ── HEADLINE — IMPORTED axis, no leak, correct value ──────────────────────
BEZEL_TEST(record_factory_param_passthrough_admitted_4295,
           imported_bare_parameter_passthrough_no_longer_leaks) {
    auto dir = wtDir4295();
    LibHandle libH = buildLib(dir, kLibSrc4295);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle appH = buildApp(dir, libH.objPath, kAppSrc4295, /*sanitize=*/true);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    RunResult rr = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(rr.code, 50);
    BEZEL_EXPECT_TRUE(rr.out.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Direct leak") == std::string::npos);
    BEZEL_EXPECT_TRUE(rr.out.find("Indirect leak") == std::string::npos);
}

// ── CONTROL — same parameter embedded twice stays rejected (no UAF) ──────
BEZEL_TEST(record_factory_param_passthrough_admitted_4295,
           double_embed_same_parameter_stays_rejected_no_uaf) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(kDoubleEmbedSrc4295, 15000));
}

// ── CONTROL — a `ref` parameter stays rejected (no UAF) ───────────────────
BEZEL_TEST(record_factory_param_passthrough_admitted_4295,
           ref_parameter_passthrough_stays_rejected_no_uaf) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(kRefParamSrc4295, 15000));
}

}  // namespace bezel::codegen
