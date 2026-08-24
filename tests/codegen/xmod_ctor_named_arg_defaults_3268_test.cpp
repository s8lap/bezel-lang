// Spec-derived regression test for issue #3268 — §22.5 NAMED-ARGUMENT calls
// against an imported constructor with default-valued parameters MUST honor
// the same binding the in-module twin uses (language spec §22.5 "Optional
// parameters and named arguments" / §2.8.2 "The artifact interface is a
// semantic requirement", v0.244 — heading verified per
// spec-citation-discipline).
//
// §22.5 establishes the optional-parameter + named-argument binding rule
// at the call site, evaluated eagerly (§28.3); §2.8.2 establishes that
// every cross-module declaration is part of the artifact interface,
// so a caller in another module observes the SAME surface — including
// default-valued parameters. The headline shape of the issue's repro
// (`new ImportedT(b := 7)` against `ImportedT(a: int := 10, b: int := 5)`)
// is legal on both sides of the §2.8.2 boundary, and #3268 closes the
// asymmetry where the cross-module twin still loud-rejected this shape
// even after #3082 lifted the all-positional half.
//
// #3268 closed the residual asymmetry that #3082 left behind: the
// all-positional + omitted-trailing case is admitted by #3082
// (`d7c84ff`); the NAMED-ARGUMENT case (`new T(b := 7)` against
// `T(a: int := 10, b: int := 5)`) is admitted by #3268. The pre-#3268
// shape: `checkImportedNewExprArgs`'s "A named/modifier argument is
// outside this conservative check." early-return short-circuited
// `bindArgsToParams` for the named-arg case, so codegen's
// imported-ctor `allPositional` gate
// (`LlvmBackend_Exprs2.cpp:5873`, gated on
// `allPositional && ne.args.size() <= cs->params.size()`) bailed,
// control fell through to the #2658 loudness guard, the `bz$T$ctor`
// call was never emitted, and a calloc'd default-zeroed `obj` was
// returned with a `internal:` warning — loud-but-wrong. The in-module
// twin lowered the same call correctly.
//
// ── WHAT IT REPLACES (measured on e4c2a7a, the parent) ──────────────
//
// Two call-path holdbacks:
//
//   1. Sema (`checkImportedNewExprArgs` at
//      `src/sema/ExprTypecheck_Calls.cpp:11468`) silently returned on
//      a named/modifier arg without running `bindArgsToParams`. The
//      comment claimed "outside this conservative check" because the
//      imported-ctor-mandatory `out`/`out owned` and the
//      `owned`-modifier obligations are out of scope here — but the
//      named-arg CALL-SHAPE (§22.5 named-only / mixed
//      named-and-positional, no modifier, no discard) was IN scope
//      and was being silently dropped. The new check accepts the
//      named-only shape and routes it through the SAME
//      `bindArgsToParams(FormalsView::of(*cs), …)` binder the
//      imported-method call path already uses (mirrors
//      `checkImportedCallArity`).
//
//   2. Codegen (`lowerNew` imported-ctor branch at
//      `src/codegen/LlvmBackend_Exprs2.cpp:5873`) gated on
//      `allPositional`, so the named-arg case left the call
//      un-emitted. The new sibling branch runs whenever the §22.5
//      binding has been published on the
//      `SemaContext::setNewExprPositionalBinding` side table (the
//      SAME side table the imported-record path's
//      `storePrimaryCtorPositionalArgs` reads via #3196), uses the
//      ctor twin of `importedFormalToArg` (`LlvmBackend_Setup.cpp`)
//      to walk formals in declaration order, fills each from its
//      bound call-site arg (placed by the side table), and
//      materializes every unfilled formal via
//      `importedCtorDefaultArgValue` — the same default-fold the
//      all-positional path uses (#3082).
//
// The all-positional path is bit-identical to #3082 — same lowering
// loop, same defaults loop, same `nullTransferredOwnedLocalSlot`
// post-pass for `owned`-transfer on the import side. The named-arg
// branch is purpose-built for the no-modifier / no-discard shape
// (the shape the binder accepts); modifier / discard shapes are
// still out of scope and remain declined by the all-positional
// gate, with their loud-but-wrong shape filed as a follow-up.
// ──────────────────────────────────────────────────────────────────────
//
// ── THE FIX SHAPE ────────────────────────────────────────────────────
//
// (1) `checkImportedNewExprArgs` removes the "outside this conservative
//     check" early-return for the named-only / mixed call shape, runs
//     `bindArgsToParams(FormalsView::of(*cs), ne.args, …)` over it,
//     and publishes `b.paramToArg` on the
//     `SemaContext::setNewExprPositionalBinding` side table. Modifier
//     and discard args still early-return (#3081 / the `out owned`
//     discard wedge is structurally separate).
//
// (2) `importedFormalToArg(*cs, ne.args)` is the ctor twin of the
//     method `importedFormalToArg(*ms, call.args)` already used by
//     `lowerCall`'s imported-method arm at
//     `src/codegen/LlvmBackend_Calls2.cpp:8233`. Same rule, same
//     identity fallback, same `formalParamCount/Name/HasDefault`
//     accessors (already overloaded for `ConstructorSymbol` in
//     `Symbol.hpp:1298-1399`).
//
// (3) `lowerNew`'s imported-ctor branch adds a SIBLING block for the
//     named-arg path. The block looks up the §22.5 binding via
//     `ctx_.newExprPositionalBinding(ne)`, computes the
//     `formalToArg` map from the imported formal list (the ctor
//     twin of the helper), lowers each call-site arg in source
//     order (§28.3) but PLACES it by formal index in an `ordered`
//     vector, and finally materializes every unfilled formal's
//     declared default through `importedCtorDefaultArgValue`.
// ──────────────────────────────────────────────────────────────────────
//
// ── ANTI-REGRESSION COVERAGE (the axis grid) ─────────────────────────
//
// T1 — HEADLINE NAMED ARG (the issue's repro):
//      `new T(b := 7)` against an imported `T(a: int := 10, b: int := 5)`.
//      Pre-fix: `internal: no constructor call could be lowered … #2658`
//      warning + a `0` return. Post-fix: `37` (= 10 + 7 + 20).
// T2 — TWO NAMED ARGS IN DECLARATION ORDER:
//      `new T(a := 20, b := 7)`. Same ctor. Pre-fix: same loudness
//      warning + `0`. Post-fix: `47` (= 20 + 7 + 20).
// T3 — TWO NAMED ARGS IN REVERSE ORDER (the §22.5 binding rule's
//      load-bearing case: argument-order independence):
//      `new T(b := 7, a := 20)`. The naïve positional reading
//      would compute 10 + 7 + 20 = 37 (NOT 47); the
//      binder-driven placement must read `paramToArg[0] = 1` and
//      `paramToArg[1] = 0` to assign a=20 / b=7. Post-fix: `47`.
//      A regression that read `ne.args[i]` directly into formal `i`
//      would emit 37 here, distinguishable from the pre-fix `0`.
// T4 — ALL-POSITIONAL ANTI-REGRESSION: `new T(20, 7)` against the
//      same ctor. Pre-#3082: rejected (`has 2 argument(s)`).
//      Post-#3082: `47`. Post-#3268: still `47`. The fix must
//      preserve the all-positional path's #3082 behavior bit-
//      identically. (This is the load-bearing anti-regression for
//      the named-arg branch's "lifting the gate" claim.)
// T5 — DEFAULT-ONLY (no args): `new T()` against the same ctor.
//      Pre-#3082: rejected. Post-#3082: `35` (= 10 + 5 + 20).
//      Post-#3268: still `35`. Pins that the named-arg branch's
//      binder publishes the same side table the default-taker uses.
// T6 — CONTROL: REQUIRED-PARAMETER REJECTION. `new T(b := 7)`
//      against `T(a: int, b: int := 5)` (no default for `a`)
//      MUST be rejected with the §22.5 message. The named-arg
//      branch's binder is `bindArgsToParams` (same body the method
//      arm uses), so an unfilled required formal produces the
//      same diagnostic the imported-METHOD call path already
//      pins (the regression test for #3213). Pinning it here
//      closes the door to a silent-zero imported-ctor widening.
//
// ── ORACLE ───────────────────────────────────────────────────────────
//
// Every row above runs under the AOT-execution oracle
// (`runCapture`'s `popen`-driven native exec, the same oracle
// #3082's regression test uses). The AOT path is fork-isolated for
// both bezelc and the linked binary, so a silent-zero defect that
// was masked by the #2658 loudness guard cannot survive: either
// the binary returns the §22.5 value, or it does not. The (T6)
// required-parameter control runs the `bezelc` CLI to capture its
// stderr (the `Driver::compileFile` in-process return only carries
// `success` / `errorCount`, not the diagnostic text), the same
// shape #3082's (d)/(e) anti-regression rows use.
//
// ── WHY THIS TEST AND NO LIT/FOR FILECHECK ───────────────────────────
//
// The #3268 defect was a runtime wrong value (zero on the imported
// side) plus a loudness warning. The runtime wrong-value half is the
// load-bearing pin and the lit/FileCheck diagnostic track cannot
// observe it (lit only confirms the diagnostic text, not the
// lowered call). The loudness-warning half is already pinned by
// `xmod_ctor_optional_params_3082_test.cpp`'s (d)/(e) rows. So a
// gtest-driven `runCapture` AOT-exec oracle is the right tool here,
// matching the surrounding test directory's existing pattern.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {

// TU-scope scratch buffer for the T6 diagnostic text (the in-process
// `Driver::compileFile` swallows stderr, so the CLI path stashes its
// text here for the test BODY to grep). Per-process, so contention
// between gtest rows in the same test binary is not an issue (each
// TEST_F spawns a new driver, and the CLI is launched once per row).
std::string lastCliStderr;

namespace {

// Per-process unique working dir under the shared scratch base (same
// rationale as the sibling tests: /tmp tmpfs races between fork'd shards
// would silently destroy the lib.o the app build needs to link against).
const std::filesystem::path& wtDir3268() {
    return wtScratchDir("wt-3268");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Run a binary and return {exitCode, combined-stdout-and-stderr}.
struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe, const std::string& args = "") {
    std::string out;
    FILE* p = popen((exe + (args.empty() ? "" : " " + args) +
                     " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Build the shared library object once per test, return its path.
//
// Lifecycle note: the `TempFile` for the .o MUST outlive any subsequent
// `buildApp` call — the importer-side `compileFile` reopens the .o to
// read its `.bezel.meta` section, and a deleted .o shows up as
// `bezelc: error: cannot load module metadata from ...`. Keeping the
// returned `std::shared_ptr` alive in the caller (alongside the matching
// app TempFiles) holds the file open for the duration of the test.
struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_3268lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

// In-process importer-side compile (uses `Driver::compileFile`).
struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3268app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = libObjPath; d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// CLI compile (so we can capture stderr — `Driver::compileFile`'s
// in-process return carries only `success`/`errorCount`, not the
// diagnostic text). Used by T6 to pin the §22.5 wording.
struct CliApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::string objPath;
};

CliApp buildAppCli(const std::filesystem::path& dir,
                   const std::string& libObjPath,
                   const std::string& appSrc) {
    CliApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3268appcli_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.objPath = (dir / (stem + "_app.o")).string();
    writeSource(r.src->path, appSrc);
    // Invoke the just-built bezelc directly so stderr is preserved;
    // `Driver::compileFile`'s in-process path swallows the diagnostic
    // text. The CLI is the same `bezelc` that lives at the worktree's
    // `build/bezelc`; resolve it relative to the cwd (the producer
    // lane's `agent_check.sh` makes `build/bezelc` available from the
    // worktree root).
    std::string cmd = "./build/bezelc -c " + r.src->path +
                      " -o " + r.objPath +
                      " --import " + libObjPath + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[256];
    std::string out;
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    r.ok = (status == 0);
    if (!r.ok) {
        // Stash stderr on a member so the caller can match against the
        // §22.5 wording. The CliApp only carries `bool ok` — to keep
        // the diagnostic text visible to the test, return it via a
        // global scratch buffer (per-process; each gtest row is a
        // separate binary spawn so contention is not an issue).
        lastCliStderr = out;
    }
    return r;
}

}  // namespace

// ── (T1) HEADLINE NAMED ARG — §22.5 / §2.8.2 — b := 7 ──────────────

// Mirror of the issue's repro, with the ctor shaped to allow either
// arg to be omitted (so `a := 10` is a default-taker, not a required
// slot): `T(a: int := 10, b: int := 5)` imported; the consumer
// calls `new T(b := 7)` and reads `w.v`. Pre-#3268:
// `checkImportedNewExprArgs`'s early-return on named args skips
// `bindArgsToParams`, codegen's imported-ctor `allPositional` gate
// rejects the call, and `lowerNew` returns a calloc'd default-zeroed
// instance with a #2658 `internal:` warning — `0` at runtime.
// Post-#3268: the §22.5 binder over `FormalsView::of(*cs)` admits
// the call, the side table publishes `paramToArg[0] = -1,
// paramToArg[1] = 0` (formal `a` is the default-taker, formal `b`
// is bound to call-site arg 0), codegen reads the side table,
// computes `importedFormalToArg(*cs, ne.args)`, places the lowered
// `7` into `ordered[1]`, materializes `10` for `ordered[0]` via
// `importedCtorDefaultArgValue(*cs, 0)`, and emits the
// `bz$T$ctor(obj, 10, 7)` call. Ctor body computes 10 + 7 + 20 =
// 37.
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           one_named_arg_default_other_spec_22_5_2_8_2) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t1; "
        "public class T "
        "  public v: int; "
        "  public constructor(a: int := 10, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end T; ";
    const std::string app =
        "module WTApp3268_t1; "
        "import WTLib3268_t1; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new T(b := 7); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 37);
}

// ── (T2) TWO NAMED ARGS IN DECLARATION ORDER — a := 20, b := 7 ───

// `new T(a := 20, b := 7)` against `T(a: int := 10, b: int := 5)`:
// 20 + 7 + 20 = 47. Pre-#3268: same loudness warning + 0. The
// semi-trivial position-pairing `ne.args[i] -> formal i` would
// happen to land this correctly (the call's source order matches
// declaration order), but the load-bearing case is T3.
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           two_named_args_decl_order_spec_22_5_2_8_2) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t2; "
        "public class T "
        "  public v: int; "
        "  public constructor(a: int := 10, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end T; ";
    const std::string app =
        "module WTApp3268_t2; "
        "import WTLib3268_t2; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new T(a := 20, b := 7); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 47);
}

// ── (T3) TWO NAMED ARGS IN REVERSE ORDER — b := 7, a := 20 ─────

// The pre-#3196 codegen was a positional-index pairing; the
// pre-#3268 call shape had no codegen at all. The headline load-
// bearing shape for the §22.5 binder in the imported-ctor path:
// `new T(b := 7, a := 20)` against `T(a: int := 10, b: int := 5)`.
// The §22.5 binder resolves `paramToArg[0] = 1` (formal `a`
// bound to call-site arg 1) and `paramToArg[1] = 0` (formal `b`
// bound to call-site arg 0); codegen's named-arg branch uses the
// same `importedFormalToArg(*cs, ne.args)` ctor twin the method arm
// already used (#3178/#3213). 20 + 7 + 20 = 47. A regression that
// read `ne.args[i]` directly into formal `i` would emit 37 here
// (10 default + 7 named-b-for-a-position + 20 = 37 with the wrong
// placement, OR 7 + 20 + 20 = 47 if the misordering lands by luck);
// the side-table-driven placement is the only path that emits 47.
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           two_named_args_rev_order_spec_22_5_2_8_2) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t3; "
        "public class T "
        "  public v: int; "
        "  public constructor(a: int := 10, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end T; ";
    const std::string app =
        "module WTApp3268_t3; "
        "import WTLib3268_t3; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new T(b := 7, a := 20); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 47);
}

// ── (T4) ALL-POSITIONAL ANTI-REGRESSION — new T(20, 7) ───────────

// The fix MUST NOT alter the all-positional path #3082 lifted. The
// named-arg branch's side-table look-up early-bails when
// `allPositional` is true, so this case never enters the new
// code, but pinning the original #3082 value at this side table
// confirms the lifting claim. 20 + 7 + 20 = 47.
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           all_positional_still_works_post_3268_spec_22_5) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t4; "
        "public class T "
        "  public v: int; "
        "  public constructor(a: int := 10, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end T; ";
    const std::string app =
        "module WTApp3268_t4; "
        "import WTLib3268_t4; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new T(20, 7); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 47);
}

// ── (T5) DEFAULT-ONLY ANTI-REGRESSION — new T() ──────────────────

// The headline shape's flipped twin: no call-site args at all, the
// §22.5 default-takers fill both formals. Pre-#3082: rejected.
// Post-#3082: 35 (= 10 + 5 + 20). Post-#3268: still 35 (this row
// is a load-bearing anti-regression for the named-arg branch's
// default-fill loop — a regression that only emitted explicit-arg
// formals would still emit 35 here; the case worth pinning is
// against a future widening that lowers `new T(a := 20)` to the
// wrong formal).
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           no_args_default_only_spec_22_5) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t5; "
        "public class T "
        "  public v: int; "
        "  public constructor(a: int := 10, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end T; ";
    const std::string app =
        "module WTApp3268_t5; "
        "import WTLib3268_t5; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new T(); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 35);
}

// ── (T6) CONTROL — required-parameter rejection under named args ─

// `new T(b := 7)` against `T(a: int, b: int := 5)` (no default for
// `a`). MUST be rejected — `a` is a required formal, and the §22.5
// binder `bindArgsToParams` reports "required parameter `a` is not
// supplied". The named-arg path's binder is the SAME body the
// method-call path uses (already pinned at the §22.5 wording level
// by #3213's regression test for imported methods); pinning it here
// for the imported-ctor path closes the door to a silent-zero
// widening. Diagnostic text is captured via the CLI's stderr
// (the in-process `Driver::compileFile` swallows the diagnostic).
BEZEL_TEST(xmod_ctor_named_arg_defaults_3268,
           named_arg_required_param_still_rejected_spec_22_5) {
    auto dir = wtDir3268();
    const std::string lib =
        "module WTLib3268_t6; "
        "public class TReq "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end TReq; ";
    const std::string app =
        "module WTApp3268_t6; "
        "import WTLib3268_t6; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new TReq(b := 7); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    CliApp r = buildAppCli(dir, libH.objPath, app);
    BEZEL_EXPECT_FALSE(r.ok);
    if (r.ok) return;
    // The §22.5 wording: "required parameter `a` is not supplied
    // (§22.5)". Match on the parameter name to ride out any future
    // rewording of the §22.5 citation.
    bool found_a_diag = lastCliStderr.find("required parameter `a`") !=
                        std::string::npos;
    BEZEL_EXPECT_TRUE(found_a_diag);
}

}  // namespace bezel::codegen
