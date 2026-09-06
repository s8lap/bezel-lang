// Spec-derived regression coverage for issue #3911 — an imported MIDDLE
// ancestor's OWN inheritance-clause base-constructor arguments are silently
// dropped when that middle ancestor is IMPORTED and GENERIC, in a THREE (or
// more) level ancestor chain: `Derived : Mid<T>(a) : Base(mp)` where `Mid` is
// declared in a dependency module and `Derived` is local.
//
// ── ROOT CAUSE (traced this lane, on r14/c15-l2, r12-cycle-1 @ 1fae852 plus
//    fix #3747 3197846) ───────────────────────────────────────────────────
//
// `emitImplicitBase` (src/codegen/LlvmBackend_Setup.cpp) and
// `prePopulateBasePositionalFields` (src/codegen/LlvmBackend_Exprs2.cpp) both
// recover a class's OWN inheritance-clause argument expressions
// (`ast::TypeDecl::primaryBaseCtorArgs`) via `dynamic_cast<const
// ast::TypeDecl*>(cls->decl)`. `sema::TypeSymbol::decl` is null for EVERY
// imported type (the #2374-class gap; see ModuleLoader.cpp's TypeSymbol-shell
// comment) — generic or not. For a class reached DIRECTLY at a `new` site or
// as the IMMEDIATE base of a LOCAL derived class this is masked: the LOCAL
// class's own decl is real, and (for a two-level chain) the recursion never
// needs to read the imported base's OWN base-clause args because that base
// has no further ancestor, or because a NON-generic imported base routes
// through its exported `bz$<Base>$init` / `bz$<Base>$initprimary` entry point
// instead of a local AST replay — and THAT entry point was compiled in the
// base's own module, where its decl is real (confirmed: T3/T4 below).
//
// The gap is reachable ONLY when the recursion must replay a MIDDLE
// ancestor's OWN base clause locally — which happens only for an IMPORTED
// GENERIC middle ancestor, because a generic import has no per-instantiation
// exported entry point (`initFns_`'s gate requires `genericParams.empty()`,
// exactly the constraint fix #3747 documents) and so MUST be replayed via the
// reconstructed-AST family (`ctorBodyText`/`fieldInitText`) — which carries
// member bodies and field initializers but has NO channel at all for
// `primaryBaseCtorArgs` (verified: no `baseCtorArgsText`-shaped field exists
// anywhere in `src/module/ModuleMetadata.hpp`, unlike `dtorBodyText`,
// `ctorBodyText`, `fieldInitText`). T1/T2 below hit this gap through BOTH
// call sites that recurse into the middle ancestor: `emitImplicitBase`'s
// bottom fallback -> `emitImportedBaseFieldInit`'s #2524 generic arm (which
// calls `emitImplicitBase(Mid, ...)` recursively, LlvmBackend_Setup.cpp:12929)
// and, independently, `prePopulateBasePositionalFields`'s own recursion
// (LlvmBackend_Exprs2.cpp:4116) — both read `Mid->decl`, both find null.
//
// ── SCOPE CORRECTION vs. the issue as filed ────────────────────────────────
//
// #3911 as filed predicted the defect "reproduces ... whether or not it is
// generic ... I have not yet measured the non-generic variant". MEASURED
// this lane (four two-module `bezelc -c` / `--import` probes, bezelc built
// from this lane's own worktree at commit 3197846 (fix #3747) plus this
// commit): the NON-GENERIC variant is ALREADY CORRECT in BOTH constructor
// shapes (no explicit ctor, and both primary+explicit forms) — `bp=7` both
// times, because `emitImportedBaseFieldInit`'s non-generic arm
// (LlvmBackend_Setup.cpp:12959) routes through the exported `bz$<Mid>$init`
// entry point, compiled with Mid's REAL local decl in Mid's own module. Only
// the GENERIC variant reproduces the drop. T3/T4 pin the non-generic
// correctness as regression controls; T1/T2 pin the generic gap as KNOWN_GAP
// rows (mirrors the `KNOWN_GAP_...` naming convention already used in this
// suite, e.g. tests/codegen/builtin_closed_ctor_surface_2994_test.cpp) so a
// future fix flips these two names and their asserted value together.
//
// ── WHY THIS LANE DOES NOT FIX IT ───────────────────────────────────────────
//
// The general fix requires a NEW wire-format channel carrying a generic
// middle ancestor's own inheritance-clause ARGUMENT EXPRESSIONS across the
// module boundary (the issue's own "Suggested fix direction": a
// `baseCtorArgsText`-shaped field on `MetaTypeDecl`, parsed and remapped by
// the SAME family that already carries `ctorBodyText`/`fieldInitText`,
// analogous to `dtorBodyText`), PLUS a place to attach the reconstructed
// argument list that both `emitImplicitBase` and
// `prePopulateBasePositionalFields` can read given only a `TypeSymbol*` (NOT
// `TypeSymbol::decl`, which the #2374-class gap deliberately keeps null for
// every import — widening that would be its own, much larger risk). That is
// exporter + wire-schema + loader + two codegen call sites, the same class of
// multi-file surface the `ctorBodyText`/`dtorBodyText` family took its own
// dedicated fixes to land soundly (see e.g. `dtorBodyText`'s own comment in
// ModuleMetadata.hpp on the byte-identical-serialization verification this
// project expects of a wire-format addition). Filed as issue #3916 with this
// evidence and the concrete design; #3911 itself is narrowed (not closed) to
// record the generic-only scope.
//
// per §22.8 step 1 "The base or chained constructor call", issue #3911
// per §22.10 (v0.294) — "This replaces an explicit `base(...)` call"
// per fix #3747 — the sibling this issue was discovered while verifying
// per #3162 — the base-clause-argument asymmetry family

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base — forked
// shards would otherwise race on one another's `.o` in /tmp (tmpfs, RAM).
const std::filesystem::path& wtDir3911() {
    return wtScratchDir("wt-3911-codegen");
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

// The dependency object. Its `TempFile` MUST outlive the dependent compile —
// `Driver::compileFile` reopens the `.o` to read its `.bezel.meta` section.
struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& libSrc) {
    LibHandle h;
    auto dir = wtDir3911();
    std::string stem = unique_stem("bezel_3911lib_");
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
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& libObjPath, const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    auto dir = wtDir3911();
    std::string stem = unique_stem("bezel_3911app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    // compileFileForTest, not a raw driver.compileFile call, so this runner
    // is reached by the gate's `--full` generated-code ASan sweep
    // (support/driver_run_support.hpp) like every other hand-rolled runner.
    bezel::CompileResult rb =
        bezel::test::compileFileForTest(driver, r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — KNOWN_GAP: imported GENERIC middle ancestor, NO explicit constructor.
// The exact shape #3911 was filed with: `Mid<T>(mp): Base(mp)`, no explicit
// ctor, reached via `Derived: Mid<int>(a)`.
// MEASURED (this lane, bezelc built at 1fae852+3747): `a=7 mp=7 bp=0 bz=22
// bx=11` — `bp` should be 7 (Mid's own base clause `Base(mp)` is never
// evaluated because `emitImportedBaseFieldInit`'s #2524 generic arm recurses
// into `emitImplicitBase(Mid, ...)`, whose `Mid->decl` is null).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3911 (KNOWN GAP — not yet fixed)
BEZEL_TEST(imported_generic_middle_ancestor_base_clause_3911,
           KNOWN_GAP_generic_middle_no_explicit_ctor_drops_own_base_clause) {
    const std::string lib =
        "module XL3911a; "
        "public class Base3911a(bp: int) "
        "  public bz: int := 22; "
        "end Base3911a; "
        "public class Mid3911a<T>(mp: int): Base3911a(mp) "
        "  public bx: int := 11; "
        "end Mid3911a; ";
    const std::string app =
        "module XM3911a; "
        "import XL3911a; "
        "class Derived3911a(a: int): Mid3911a<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} mp={mp} bp={bp} bz={bz} bx={bx}\"); "
        "  end Show; "
        "end Derived3911a; "
        "class C3911a "
        "  public static method Main(): int "
        "    using var d := new Derived3911a(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C3911a; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    // Pins the CURRENT (wrong) behavior: mp propagates, bp does not. When
    // #3911's exporter/loader half (filed as #3916) lands, this row must be
    // renamed (drop KNOWN_GAP_) and re-asserted as "a=7 mp=7 bp=7 bz=22 bx=11".
    BEZEL_EXPECT_TRUE(
        out.find("a=7 mp=7 bp=0 bz=22 bx=11") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — KNOWN_GAP: imported GENERIC middle ancestor, BOTH primary and an
// explicit constructor. Same root cause as T1, reached the same way — the
// middle ancestor's OWN base clause is dropped regardless of whether it also
// declares an explicit constructor (fix #3747 only addressed the CALLER's
// resolution of the middle ancestor itself, one level up; it does not touch
// the middle ancestor's OWN base-clause propagation, which is #3911's
// subject).
// MEASURED (this lane): `a=7 mp=7 bp=0 bz=22 bx=11` — identical failure to T1.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3911 (KNOWN GAP — not yet fixed)
BEZEL_TEST(imported_generic_middle_ancestor_base_clause_3911,
           KNOWN_GAP_generic_middle_both_ctor_forms_drops_own_base_clause) {
    const std::string lib =
        "module XL3911b; "
        "public class Base3911b(bp: int) "
        "  public bz: int := 22; "
        "end Base3911b; "
        "public class Mid3911b<T>(mp: int): Base3911b(mp) "
        "  public bx: int := 11; "
        "  public constructor() "
        "    this(0); "
        "  end constructor; "
        "end Mid3911b; ";
    const std::string app =
        "module XM3911b; "
        "import XL3911b; "
        "class Derived3911b(a: int): Mid3911b<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} mp={mp} bp={bp} bz={bz} bx={bx}\"); "
        "  end Show; "
        "end Derived3911b; "
        "class C3911b "
        "  public static method Main(): int "
        "    using var d := new Derived3911b(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C3911b; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(
        out.find("a=7 mp=7 bp=0 bz=22 bx=11") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — REGRESSION CONTROL: imported NON-generic middle ancestor, NO explicit
// constructor. Confirms the scope correction above: this shape is ALREADY
// CORRECT (`bp=7`) because `emitImportedBaseFieldInit`'s non-generic arm
// routes through the exported `bz$<Mid>$init` entry point instead of
// replaying `Mid`'s AST locally.
// MEASURED (this lane): `a=7 mp=7 bp=7 bz=22 bx=11`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3911 (regression control — non-generic
// scope correction)
BEZEL_TEST(imported_generic_middle_ancestor_base_clause_3911,
           control_non_generic_middle_no_explicit_ctor_propagates_base_clause) {
    const std::string lib =
        "module XL3911c; "
        "public class Base3911c(bp: int) "
        "  public bz: int := 22; "
        "end Base3911c; "
        "public class Mid3911c(mp: int): Base3911c(mp) "
        "  public bx: int := 11; "
        "end Mid3911c; ";
    const std::string app =
        "module XM3911c; "
        "import XL3911c; "
        "class Derived3911c(a: int): Mid3911c(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} mp={mp} bp={bp} bz={bz} bx={bx}\"); "
        "  end Show; "
        "end Derived3911c; "
        "class C3911c "
        "  public static method Main(): int "
        "    using var d := new Derived3911c(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C3911c; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(
        out.find("a=7 mp=7 bp=7 bz=22 bx=11") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — REGRESSION CONTROL: imported NON-generic middle ancestor, BOTH primary
// and an explicit constructor. Same scope correction as T3, for the
// both-forms shape: routes through `bz$<Mid>$initprimary` (fix #3713), also
// already correct.
// MEASURED (this lane): `a=7 mp=7 bp=7 bz=22 bx=11`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3911 (regression control — non-generic
// scope correction)
BEZEL_TEST(imported_generic_middle_ancestor_base_clause_3911,
           control_non_generic_middle_both_ctor_forms_propagates_base_clause) {
    const std::string lib =
        "module XL3911d; "
        "public class Base3911d(bp: int) "
        "  public bz: int := 22; "
        "end Base3911d; "
        "public class Mid3911d(mp: int): Base3911d(mp) "
        "  public bx: int := 11; "
        "  public constructor() "
        "    this(0); "
        "  end constructor; "
        "end Mid3911d; ";
    const std::string app =
        "module XM3911d; "
        "import XL3911d; "
        "class Derived3911d(a: int): Mid3911d(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} mp={mp} bp={bp} bz={bz} bx={bx}\"); "
        "  end Show; "
        "end Derived3911d; "
        "class C3911d "
        "  public static method Main(): int "
        "    using var d := new Derived3911d(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C3911d; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(
        out.find("a=7 mp=7 bp=7 bz=22 bx=11") != std::string::npos);
}

}  // namespace bezel::codegen
