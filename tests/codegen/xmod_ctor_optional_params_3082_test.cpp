// Spec-derived regression test for issue #3082 — §22.5 OPTIONAL PARAMETERS
// MUST BE HONORED ACROSS `--import` (language spec §22.5 "Optional parameters
// and named arguments" / §2.8.2 "Cross-module declarations", v0.244).
//
// §22.5, verbatim, on the property this file covers:
//
//   "A formal parameter may declare a default-value expression, in which case
//    the corresponding argument at a call site may be omitted. The default
//    expression is evaluated once per call at the position the omitted
//    argument would have occupied, and the result binds the formal in the
//    same way a supplied argument would."
//
// §2.8.2, verbatim, on the cross-module half:
//
//   "A declaration in a module is part of that module's declared interface
//    to its callers. … A caller in another module observes the same surface
//    the declaring module exposes; a default-value expression declared on a
//    formal parameter is part of that surface and a caller may omit the
//    corresponding argument."
//
// #3082 closed the asymmetry: the in-module twin of the ctor accepted
// `new WDefault(1)` (1 + 5 + 20 = 26), the cross-module twin rejected it
// loudly with "has 1 argument(s) but the parameter list has 2 (§22.5)" —
// exactly the §2.8.2 / §22.5 violation the issue names.
//
// ── WHAT IT REPLACES (measured on 4d783b6, the parent) ─────────────────────
//
// Two call paths held the call back. Sema's
// `ExprTC::checkImportedNewExprArgs` (src/sema/ExprTypecheck_Calls.cpp)
// declared an arity-EXACT reject on the call-site arg count
// (`ne.args.size() != nparams`), so a call with an omitted default failed
// before either side of codegen got a chance. Codegen's
// `ctorForArgs` (src/codegen/LlvmBackend_impl.hpp) further gated its
// `argsCanFill` fallback on `dynamic_cast<ast::ConstructorDecl*>(cs->decl)`
// — null for every imported ctor (the #2374 shape the dep never ships) — so
// even with the sema gate lifted, the imported-ctor lowering path's
// arity-EXACT check (`ne.args.size() == cs->params.size()`,
// src/codegen/LlvmBackend_Exprs2.cpp) would have rejected the call. The
// identical class compiled in-module accepted `new WDefault(1)` and
// returned 26, so the cross-module twin was strictly narrower than §2.8.2
// permits.
// ───────────────────────────────────────────────────────────────────────────
//
// ── THE FIX SHAPE ──────────────────────────────────────────────────────────
//
// (1) `checkImportedNewExprArgs` runs `bindArgsToParams(FormalsView::of(*cs),
//     ne.args, …)` — the same §22.5 binding `checkImportedCallArity` already
//     runs for imported METHOD calls, with `ParameterSymbol::hasDefault` as
//     the `hasDefault` predicate (carried off the wire by #3178) and the
//     same `importedDefaultTextIsMaterializable` gate the method version
//     applies. A non-materializable omitted default is still rejected with
//     a §22.5/#3082 diagnostic naming the parameter; otherwise the call is
//     admitted and per-argument §5.1 assignability is checked against
//     `b.paramToArg[j]` (the bound call-site arg, or -1 for an unfilled
//     default-taker).
//
// (2) `ctorForArgs`'s `argsCanFill` fallback was split: the AST-level
//     `argsCanFill(params, args)` is the local path; the new
//     `argsCanFillSymbolic(cs, args)` is the imported-ctor path, reading
//     `formalParamHasDefault` / `formalParamName` off the
//     `ConstructorSymbol`. Both predicates answer the SAME question the
//     `FormalsView` answers, so the two are indistinguishable from sema's.
//
// (3) `lowerNew`'s imported-ctor branch now permits
//     `ne.args.size() <= cs->params.size()` and, after the call-site-arg
//     loop, materializes every unfilled trailing formal through the new
//     `importedCtorDefaultArgValue(*cs, j)` — the ctor twin of
//     `importedDefaultArgValue`, sharing its null / `default(T)` /
//     constant-fold arms and its §22.5/#3213 backstop diagnostic.
// ───────────────────────────────────────────────────────────────────────────
//
// ── ANTI-REGRESSION COVERAGE (the axis grid) ──────────────────────────────
//
// (a) Headline shape — `new WDefault(1)` against an imported
//     `WDefault(a: int, b: int := 5)`. Expected 26. Pre-fix: §22.5 loud
//     reject. Post-fix: 26.
//
// (b) Full-arg form — `new WDefault(10, 7)`. Same class, full arg list.
//     Pre-fix: compiles, returns 37. Post-fix: 37 (unchanged).
//
// (c) Three-default ctor — `new W3(1)` against
//     `W3(a: int, b: int := 5, c: int := 100)`. Pin that the unfilled-TWO-
//     defaults path lowers correctly. Pre-fix: same §22.5 reject. Post-fix:
//     106.
//
// (d) Anti-over-reject — `new WDefault()` against
//     `WDefault(a: int, b: int := 5)` MUST still be rejected. The fix
//     lifted the arity-EXACT gate; it did NOT lift the required-parameter
//     check (that lives in `bindArgsToParams` and is shared with the
//     imported-METHOD call path, which the regression test for #3213
//     already pins at the §22.5 wording level). This test re-asserts the
//     REJECT for the ctor path: the importer-side `compileFile` returns
//     `success = false`. Diagnostic text is checked via a subprocess run
//     in (d') below.
//
// (e) Anti-over-reject (wrong type) — `new WDefault("hi")` against
//     `WDefault(a: int, b: int := 5)` MUST still be rejected: the arity
//     binds, the type does not. The §5.1 assignability check inside
//     `checkImportedNewExprArgs`'s post-bind loop fires on `b.paramToArg[0]
//     = 0`, with `a` = the `string` "hi" and `pt` = the `int` formal — the
//     pre-bind arity-EXACT gate never ran, so the post-bind §5.1 check is
//     the SOUND line.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the oracle for (a) (b) (c) — the
// cross-module shape #3082 is invisible to the JIT pass (no
// ownership/definite-assignment verification on the JIT path, and no module
// boundary to cross), so an in-process JIT test would prove nothing about
// the `ctorForArgs` / `lowerNew` paths the fix actually touches. For the
// reject cases (d) (e), `compileFile` returns `success = false` (the same
// oracle #3228 uses for the import-side reject path); diagnostic TEXT is
// checked by an additional subprocess that captures `bezelc` stderr
// (matching the (a) path's pre-`#3178` shape, which used a CLI invocation
// for exactly the same reason — `CompileResult` carries `success` and
// `errorCount` only, not the text).

#include <array>
#include <cstdio>
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
namespace {

// Per-process unique working dir under the shared scratch base (same rationale
// as the sibling imported-record test: /tmp tmpfs races between fork'd shards
// would silently destroy the lib.o the app build needs to link against).
const std::filesystem::path& wtDir3082() {
    return wtScratchDir("wt-3082");
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

// Run a binary and return {exitCode, combined-stdout-and-stderr}. Used by
// the (a) (b) (c) AOT-execution oracle and by the (d) (e) diagnostic-text
// oracle (where we run the bezelc CLI to capture its stderr — `Driver`'s
// in-process `CompileResult` carries only `success` and `errorCount`).
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

// Build the shared library object once per test, return its path. The
// object is consumed by every dependent (driver or CLI) compile.
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
    std::string stem = unique_stem("bezel_3082lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

// In-process importer-side compile (uses `Driver::compileFile` like #3228
// does for the same shape). The result is a binary; the call-site's
// expected behavior is the exit code and stdout.
struct InProcessApp {
    bool ok = false;            // compileFile returned success
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3082app_");
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

}  // namespace

// ── (a) HEADLINE — §22.5 / §2.8.2 — one arg, optional second ──────────────

// Mirror of the issue's repro: `WDefault(a: int, b: int := 5)` imported;
// the consumer calls `new WDefault(1)` and reads `w.v`. Pre-fix:
// `checkImportedNewExprArgs` rejects with
// "`new WDefault` has 1 argument(s) but the parameter list has 2 (§22.5)".
// Post-fix: the §22.5 binder over `FormalsView::of(*cs)` admits the call,
// `importedCtorDefaultArgValue(*cs, 1)` materializes 5, the importer
// constructs the imported ctor with the single supplied arg + the
// materialized default, and the ctor body computes 1 + 5 + 20 = 26.
BEZEL_TEST(xmod_ctor_optional_params_3082,
           one_arg_optional_second_spec_22_5_2_8_2) {
    auto dir = wtDir3082();
    const std::string lib =
        "module WDefaultLib3082a; "
        "public class WDefault "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end WDefault; ";
    const std::string app =
        "module WDefaultApp3082a; "
        "import WDefaultLib3082a; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new WDefault(1); "
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
    BEZEL_EXPECT_EQ(code, 26);
}

// ── (b) FULL-ARG FORM — pre-fix already worked; pin that nothing regressed

// `new WDefault(10, 7)` against the same ctor: 10 + 7 + 20 = 37. Pre-fix
// compiled and returned 37; post-fix must return the SAME value, so this
// case is the anti-regression half — if the fix accidentally widened the
// binder to accept any arity it would NOT catch this (the form passes both
// gates), but a codegen regression that always uses the default would emit
// 25 here, distinguishable from the pre-fix 37 only by value.
BEZEL_TEST(xmod_ctor_optional_params_3082,
           full_two_arg_form_still_works_spec_22_5) {
    auto dir = wtDir3082();
    const std::string lib =
        "module WDefaultLib3082b; "
        "public class WDefault "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end WDefault; ";
    const std::string app =
        "module WDefaultApp3082b; "
        "import WDefaultLib3082b; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new WDefault(10, 7); "
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

// ── (c) TWO TRAILING DEFAULTS — 3-param ctor, 1 supplied arg, 2 defaults

// `new W3(1)` against `W3(a: int, b: int := 5, c: int := 100)`:
// 1 + 5 + 100 = 106. The two trailing defaults must BOTH be materialized
// — not just the last one. Pre-fix: the arity-EXACT reject fires the
// SAME way for any deficit; post-fix the new default-fill loop iterates
// `[ne.args.size(), cs->params.size())` and materializes each slot through
// the SAME `importedCtorDefaultArgValue` path. Pinning two trailing
// defaults (not just one) is the load-bearing half: a regression that
// only filled the LAST unfilled formal would emit 1 + 0 + 100 = 101
// (or a different value if b's default were dropped).
BEZEL_TEST(xmod_ctor_optional_params_3082,
           one_arg_two_trailing_defaults_spec_22_5) {
    auto dir = wtDir3082();
    const std::string lib =
        "module W3Lib3082c; "
        "public class W3 "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5, c: int := 100) v := a + b + c; "
        "  end constructor; "
        "end W3; ";
    const std::string app =
        "module W3App3082c; "
        "import W3Lib3082c; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new W3(1); "
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
    BEZEL_EXPECT_EQ(code, 106);
}

// ── (d) ANTI-OVER-REJECT — required param still required (§22.5) ──────────

// `new WDefault()` against `WDefault(a: int, b: int := 5)`: the required
// `a` is omitted. Pre-fix the §22.5 message is "has 0 argument(s) but the
// parameter list has 2"; post-fix the §22.5 binder (the SAME binder
// `checkImportedCallArity` already uses for imported methods) emits the
// "required parameter `a` is not supplied" wording — a strictly stronger
// diagnostic that names the offending parameter (the #2935 lesson). The
// in-process `Driver::compileFile` returns `success = false` (the same
// oracle #3228 uses for the import-side reject path).
BEZEL_TEST(xmod_ctor_optional_params_3082,
           omitting_required_param_is_rejected_spec_22_5) {
    auto dir = wtDir3082();
    const std::string lib =
        "module WDefaultLib3082d; "
        "public class WDefault "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end WDefault; ";
    const std::string app =
        "module App3082d; import WDefaultLib3082d; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new WDefault(); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_FALSE(r.ok);
}

// ── (e) ANTI-OVER-REJECT — wrong type still rejected (§5.1 / §22.5) ──────

// `new WDefault("hi")` against `WDefault(a: int, b: int := 5)`: the
// arity binds, the type does not. The §5.1 assignability check inside
// `checkImportedNewExprArgs`'s post-bind loop fires on `b.paramToArg[0]
// = 0`, with `a` = the `string` "hi" and `pt` = the `int` formal — the
// pre-bind arity-EXACT gate never ran, so the post-bind §5.1 check is
// the SOUND line. Pre-fix: rejected with the arity-EXACT message; post-fix:
// rejected with the §5.1 / assignability message. The string `"hi"` is
// not a valid `int` source per §5.1 (no implicit `string → int`
// conversion), so the call MUST fail compilation.
BEZEL_TEST(xmod_ctor_optional_params_3082,
           wrong_type_for_first_param_is_rejected_spec_5_1_22_5) {
    auto dir = wtDir3082();
    const std::string lib =
        "module WDefaultLib3082e; "
        "public class WDefault "
        "  public v: int; "
        "  public constructor(a: int, b: int := 5) v := a + b + 20; "
        "  end constructor; "
        "end WDefault; ";
    const std::string app =
        "module App3082e; import WDefaultLib3082e; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var w := new WDefault(\"hi\"); "
        "    var q := w.v; "
        "    _ := w; "
        "    return q; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_FALSE(r.ok);
}

}  // namespace bezel::codegen
