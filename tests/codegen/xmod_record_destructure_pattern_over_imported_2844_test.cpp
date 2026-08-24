// fix #2844 — §19.3 / §27.4 / §2.8.3 rule 4 (v0.240): a record DESTRUCTURING
// pattern over an IMPORTED record emits malformed IR (no terminator on
// `case.end`) / a hard sema error, because every code path that resolves the
// pattern's head record TypeSymbol walked `ctx_.symbols()` only — which is
// local-only (SemaContext.hpp:90's `symbols_`), so an imported record's
// TypeSymbol was never found. With `recTy` null the codegen path returned
// `nullptr`, the case-pattern caller treated that as "degraded to no-match"
// and skipped emitting the binding alloca for the arm body's `x`/`y` reads;
// the body lowered, but its reads of never-defined locals stranded `case.end`
// without a terminator (the issue's exact IR-verification error). The sema
// twin (`bindLambdaDestructure` and the two `fieldsOfRecord` nested-pattern
// helpers in CaseStmt / CaseExpr) hit the same defect on the typecheck side.
//
// §19.3, verbatim, on the property this file covers:
//
//   "Record destructuring: `Point(x, y)`, `MyRecord(a, b, c)`. The type name
//    followed by parenthesized bindings matches the type and binds positional
//    fields by position. The number of bindings must match the record's
//    positional-field count."
//
// §27.4, verbatim (Lambda-parameter record destructuring):
//
//   "The head must name a positional record type, and it must be *exactly*
//    the parameter's type: when a target type supplies the parameter type,
//    a head naming any other type is a compile error (no subtype test,
//    nothing to fail at runtime); when no target type is available, the
//    head *is* the annotation."
//
// §2.8.3 rule 4 (Transitive dependencies and cross-artifact type identity,
// v0.278): a type is identified program-wide by its declaring module name,
// its qualified type name, and its generic arity; one type at runtime. The
// cross-module resolution principle this fix relies on is the same one the
// codebase conventionally cites as §2.8.3 rule 4 when walking imported
// TypeSymbols (Disposal.cpp:2932, Assign.cpp:4870/4894/6112/6394, Stmts.cpp:
// 334/3990, Calls2.cpp:10298/10305/12552) — name resolution against an
// imported module's public surface MUST see the imported types, not just
// the local module's. The relevant pre-fix defect here was the LOCAL-ONLY
// lookup failing to see the imported record TypeSymbol.
//
// ── WHAT IT REPLACES ───────────────────────────────────────────────────────
//
// Two codegen call sites + three sema call sites were all walking
// `ctx_.symbols()` only:
//
//   codegen/LlvmBackend_Exprs2.cpp patternMatches (the issue's line 8530)
//   codegen/LlvmBackend_Exprs2.cpp bindLambdaDestructureParam (8785)
//   sema/ExprTypecheck_Exprs.cpp bindLambdaDestructure (415)
//   sema/ExprTypecheck.cpp CaseStmt fieldsOfRecord lambda
//   sema/ExprTypecheck_Exprs.cpp CaseExpr fieldsOfRecord lambda
//
// The fix routes each through `ctx_.importedSymbols()` after the local pass,
// mirroring the established pattern in LlvmBackend_Stmts.cpp:7657-7666 and
// LlvmBackend_Assign.cpp:10008-10020 (the same widening the #3197 fix
// applied for the §29.2 enumerable case, and the #2796 fix applied to the
// record's boxing path).
//
// ── ANTI-REGRESSION COVERAGE ───────────────────────────────────────────────
//
// (a) Headline shape — issue #2844's verbatim repro: `case o of Pt(x, y) => …`
//     over `var o: Object := p;` with `p: Pt` imported from `Lib`. Pre-fix:
//     `IR verification failed … does not have terminator! label %case.end`.
//     Post-fix: 3 + 4 = 7.
//
// (b) Static-typed scrutinee variant — `case p of Pt(x, y) => …` with the
//     scrutinee statically `Pt` (not boxed through Object). Same defect
//     class, same fix path: codegen's `patternMatches` does the head lookup
//     regardless of the scrutinee's static type, so an imported record
//     fails identically. Pre-fix: same IR-verification failure. Post-fix: 7.
//
// (c) Lambda-parameter record destructuring (§27.4) — `lambda(Pt(x, y)) => x + y`
//     where `Pt` is imported. Pre-fix: hard sema error
//     "`Pt` in a destructuring lambda parameter does not name a known
//     positional record type (§27.4)" — the sema `bindLambdaDestructure` site
//     walked the local table only. Post-fix: 7.
//
// (d) Discard slot — `case p of Pt(x, _) => x, _ => 0`. The `_` slot is
//     present and the destructure must still find the imported record to
//     know its positional field count for the binding layout. Pre-fix:
//     same IR-verification failure. Post-fix: 3 (just `x`).
//
// (e) Anti-regression — the in-module twin of (a). Pre-fix: 7 (already
//     worked). Post-fix: 7 (unchanged). Pins that the widening did not
//     break the local-record path.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the oracle for all five cases —
// the cross-module shape #2844 is invisible to the JIT pass (no module
// boundary to cross) and the JIT path skips `verifyModule` (the IR
// verification is what the issue names), so an in-process JIT test would
// prove nothing about the `patternMatches` / `bindLambdaDestructureParam`
// paths the fix actually touches. `runAot` is single-module; the multi-
// module cases (a) (b) (c) (d) use `Driver::compileObject` + `compileFile`
// with a `Dependency` (the same shape #3082 / #3228 use for their import-
// side oracle). Case (e) uses `runAot` (single-module in-module twin).

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base (same
// rationale as #3082 / #3228: /tmp tmpfs races between fork'd shards would
// silently destroy the lib.o the app build needs to link against).
const std::filesystem::path& wtDir2844() {
    return wtScratchDir("wt-2844");
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

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_2844lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

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
    std::string stem = unique_stem("bezel_2844app_");
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

// ── (a) HEADLINE — issue #2844's verbatim repro ─────────────────────────────

// `var p: Pt := new Pt(3, 4); var o: Object := p; case o of Pt(x, y) => x+y, _ => 0`.
// Pre-fix: `IR verification failed … does not have terminator! label %case.end`.
// Post-fix: 3 + 4 = 7.
BEZEL_TEST(xmod_record_destructure_pattern_over_imported_2844,
           case_object_scrutinee_headline_spec_19_3_2_8_3) {
    auto dir = wtDir2844();
    const std::string lib =
        "module PtLib2844a; "
        "public record Pt(X: int, Y: int); ";
    const std::string app =
        "module App2844a; "
        "import PtLib2844a; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(3, 4); "
        "    var o: Object := p; "
        "    return (case o of Pt(x, y) => x + y, _ => 0); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 7);
}

// ── (b) STATIC-TYPED SCRUTINEE — same defect, no Object boxing ────────────

// `var p: Pt := new Pt(3, 4); case p of Pt(x, y) => x+y, _ => 0`. The scrutinee
// is statically `Pt` (not boxed through Object). codegen's `patternMatches`
// does the head lookup regardless of the scrutinee's static type, so the
// same defect fires. Pre-fix: same IR-verification failure. Post-fix: 7.
BEZEL_TEST(xmod_record_destructure_pattern_over_imported_2844,
           case_static_typed_scrutinee_spec_19_3_2_8_3) {
    auto dir = wtDir2844();
    const std::string lib =
        "module PtLib2844b; "
        "public record Pt(X: int, Y: int); ";
    const std::string app =
        "module App2844b; "
        "import PtLib2844b; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(3, 4); "
        "    return (case p of Pt(x, y) => x + y, _ => 0); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 7);
}

// ── (c) LAMBDA-PARAMETER RECORD DESTRUCTURING (§27.4) ─────────────────────

// `var f := lambda(Pt(x, y)) => x + y; f(p)` where `Pt` is imported. Pre-fix:
// hard sema error "`Pt` in a destructuring lambda parameter does not name a
// known positional record type (§27.4)" — the sema `bindLambdaDestructure`
// site walked the local table only. Post-fix: 3 + 4 = 7.
BEZEL_TEST(xmod_record_destructure_pattern_over_imported_2844,
           lambda_parameter_destructure_spec_27_4_2_8_3) {
    auto dir = wtDir2844();
    const std::string lib =
        "module PtLib2844c; "
        "public record Pt(X: int, Y: int); ";
    const std::string app =
        "module App2844c; "
        "import PtLib2844c; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(3, 4); "
        "    var f: (Pt) -> int := lambda(Pt(x, y)) => x + y; "
        "    return f(p); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 7);
}

// ── (d) DISCARD SLOT — `_` still binds nothing ────────────────────────────

// `case p of Pt(x, _) => x, _ => 0`. The `_` slot is present in the pattern
// and the destructure must still find the imported record to know the
// positional-field count (and thus which index `x` reads). Pre-fix: same
// IR-verification failure (codegen path). Post-fix: 3 (only `x`, `_` binds
// nothing — §19.3 verbatim).
BEZEL_TEST(xmod_record_destructure_pattern_over_imported_2844,
           case_with_discard_slot_spec_19_3) {
    auto dir = wtDir2844();
    const std::string lib =
        "module PtLib2844d; "
        "public record Pt(X: int, Y: int); ";
    const std::string app =
        "module App2844d; "
        "import PtLib2844d; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(3, 4); "
        "    return (case p of Pt(x, _) => x, _ => 0); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 3);
}

// ── (e) ANTI-REGRESSION — in-module twin must still pass ──────────────────

// Identical to (a) but `Pt` declared in the SAME module as the case. This
// case pre-fix passed (the local-symbol lookup found `Pt`), and post-fix
// must continue to pass (the local pass is walked FIRST and breaks early,
// so the new `importedSymbols()` pass never enters). Pinning this rules out
// a regression where the widening accidentally preferred the imported path
// over the local one.
BEZEL_TEST(xmod_record_destructure_pattern_over_imported_2844,
           in_module_twin_still_works_spec_19_3) {
    auto prog =
        "module M2844e; "
        "record Pt(X: int, Y: int); "
        "class MainClass public static method Main(): int "
        "  var p: Pt := new Pt(3, 4); "
        "  var o: Object := p; "
        "  return (case o of Pt(x, y) => x + y, _ => 0); "
        "end Main; end MainClass;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

}  // namespace bezel::codegen
