// fix #2845 — §19.3 / §2.8.3 rule 4 (v0.240): a BARE-NAME §19.3 pattern — a
// type-test pattern (`case o of Dog`) or a named-const pattern (`case n of K.Limit`
// or `case n of Limit`) — over an IMPORTED declaration is refused by the
// backend with #1847's "is not supported by the backend: it did not resolve to
// a type, enum constant, or named constant to match against" diagnostic.
//
// Both codegen resolvers walked `ctx_.symbols()` only — which is local-only
// (SemaContext.hpp:90's `symbols_`), so an imported class/interface's
// TypeSymbol and an imported class's ConstMemberSymbol were never found. The
// descriptor lookup (`descriptorForPatternName`,
// LlvmBackend_Setup.cpp:5163) and the const-value fold
// (`constValueForPatternName`, LlvmBackend_Setup.cpp:5199) both returned null,
// and the `patternMatches` caller in LlvmBackend_Exprs2.cpp:9128 reached the
// #1847 fallback that emits a clean never-match. The `ascribed` form
// `of var p: Dog => …` and the `is` form `o is Dog` BOTH work on an imported
// class today (they are pinned by `xmod_imported_class_vtable_2792`); those
// spellings carry an `ast::Type` node with a `resolvedType` entry. The
// bare-name form carries only a `QualifiedName`, which codegen must resolve
// itself — and it was resolving local-only.
//
// §19.3, verbatim, on the property this file covers:
//
//   "Type patterns without binding: `string`, `int`, `Dog`. The bare-name
//    pattern tests the runtime type of the scrutinee against the named type
//    (a class, interface, or built-in type). Subtypes match: a Dog value
//    matches an Animal pattern. … Constants: a bare-name pattern can also
//    name a compile-time const; the pattern compares equal to its value."
//
// §2.8.3 rule 4 (Transitive dependencies and cross-artifact type identity,
// v0.278): a type is identified program-wide by its declaring module name,
// its qualified type name, and its generic arity; one type at runtime.
// §2.5 makes an imported type's use ordinary.
//
// ── WHAT IT REPLACES ───────────────────────────────────────────────────────
//
// Two codegen call sites were all walking `ctx_.symbols()` only:
//
//   codegen/LlvmBackend_Setup.cpp descriptorForPatternName (5163)
//   codegen/LlvmBackend_Setup.cpp constValueForPatternName (5199)
//
// The fix routes each through `ctx_.importedSymbols()` after the local pass,
// mirroring the established pattern in LlvmBackend_Stmts.cpp:7657-7666 and
// LlvmBackend_Assign.cpp:10008-10020, and the widening the #2844 fix applied
// to the DestructuringPattern head lookup. The `constValueForPatternName`
// fold lambda is widened: an imported ConstMemberSymbol has `decl == nullptr`
// (ModuleLoader never re-parses non-generic const bodies), so the existing
// local `lowerExpr` path returned null; the widening routes through
// `ctx_.importedConstValue(cs)` + `foldImportedConstText` (the same helper
// `constMemberValue` uses for an imported const READ).
//
// ── ANTI-REGRESSION COVERAGE ───────────────────────────────────────────────
//
// (a) Headline shape — issue #2845 repro A: `case o of Dog => …, _ => …` over
//     `var o: Object := d;` with `class Dog` imported from `Lib`. Pre-fix:
//     #1847 "is not supported by the backend". Post-fix: 11 (Dog arm matches).
//
// (b) Headline shape — issue #2845 repro B: `case n of K.Limit => …, _ => …`
//     where `class K { public const Limit: int := 7; }` is imported. Pre-fix:
//     #1847 "is not supported by the backend". Post-fix: 11 (K.Limit arm matches).
//
// (c) Bare-name imported TYPE pattern with NO terminator (the headline shape
//     but missing `_ =>`): sema emits §19.6 exhaustiveness warning, codegen
//     emits the typed arm. Pre-fix: #1847. Post-fix: 11 (the arm runs; the
//     warning is the sema-level guard, not a code-gen failure).
//
// (d) Two-import three-way dispatch: `case o of Dog => 11, Cat => 33, _ => 22`
//     where `Dog` and `Cat` are imported from DIFFERENT modules. Verifies that
//     the imported-symbols widening handles MULTIPLE imported class patterns
//     in one `case`, each routing through its own imported TypeSymbol and its
//     own `descArray_` entry. Pre-fix: every imported name reached #1847.
//     Post-fix: 11 (Dog wins for a Dog).
//
// (e) Anti-regression — local-class bare-name pattern with terminator: same
//     shape as (a) but `Dog` declared in the SAME module. Pre-fix: 11 (already
//     worked). Post-fix: 11 (the local pass is walked FIRST and breaks early,
//     so the new imported-symbols pass never enters). Pins that the widening
//     did not break the local path.
//
// (f) Anti-regression — local-class named-const pattern with terminator:
//     `case n of K.Limit => …, _ => …` where `K` is declared locally.
//     Pre-fix: 11. Post-fix: 11.
//
// ── ORACLE CHOICE ──────────────────────────────────────────────────────────
//
// AOT execution (compile + link + run) is the oracle for all six cases —
// the cross-module shape #2845 is invisible to the JIT pass (no module
// boundary to cross) and the JIT path skips `verifyModule` (the IR
// verification the issue's #1847 diagnostic can lead to), so an in-process
// JIT test would prove nothing about the `descriptorForPatternName` /
// `constValueForPatternName` paths the fix actually touches. `runAot` is
// single-module; the multi-module cases (a) (b) (c) (d) use
// `Driver::compileObject` + `compileFile` with a `Dependency` (the same
// shape #3082 / #3228 / the sibling #2844 fix use for their import-side
// oracle). Cases (e) (f) use `runAot` (single-module in-module twins).

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
// rationale as #3082 / #3228 / #2844: /tmp tmpfs races between fork'd shards
// would silently destroy the lib.o the app build needs to link against).
const std::filesystem::path& wtDir2845() {
    return wtScratchDir("wt-2845");
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
    std::string stem = unique_stem("bezel_2845lib_");
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
    std::string stem = unique_stem("bezel_2845app_");
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

// ── (a) HEADLINE A — bare-name imported CLASS pattern (with terminator) ──

// `var o: Object := new Dog(7); return (case o of Dog => 11, _ => 22)`. `Dog`
// is imported. Pre-fix: #1847 "is not supported by the backend: … (§19.3)".
// Post-fix: 11 (Dog arm matches; the ascripted form `of var p: Dog => …` has
// worked since #2792).
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           case_imported_class_bare_name_spec_19_3_2_8_3) {
    auto dir = wtDir2845();
    const std::string lib =
        "module Lib2845a; "
        "public class Dog "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int => _n; "
        "end Dog; ";
    const std::string app =
        "module App2845a; "
        "import Lib2845a; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var d: Dog := new Dog(7); "
        "    var o: Object := d; "
        "    return (case o of Dog => 11, _ => 22); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 11);
}

// ── (b) HEADLINE B — qualified imported CONST pattern (with terminator) ──

// `case n of K.Limit => 11, _ => 22` where `K` is imported and
// `public const Limit: int := 7`. Pre-fix: #1847. Post-fix: 11.
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           case_imported_qualified_const_spec_19_3_2_8_3) {
    auto dir = wtDir2845();
    const std::string lib =
        "module Lib2845b; "
        "public class K "
        "  public const Limit: int := 7; "
        "end K; ";
    const std::string app =
        "module App2845b; "
        "import Lib2845b; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var n: int := 7; "
        "    return (case n of K.Limit => 11, _ => 22); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 11);
}

// ── (c) IMPORTED CLASS BARE-NAME — no terminator ──────────────────────────

// `case o of Dog => 11` (no `_ => …`). Sema emits a §19.6 exhaustiveness
// warning; codegen emits the typed arm. Pre-fix: #1847 (the §19.6 warning is
// post-pattern-resolution, so it cannot fire if codegen rejects the pattern).
// Post-fix: 11.
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           case_imported_class_bare_name_no_terminator_spec_19_3) {
    auto dir = wtDir2845();
    const std::string lib =
        "module Lib2845c; "
        "public class Dog "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Dog; ";
    const std::string app =
        "module App2845c; "
        "import Lib2845c; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var d: Dog := new Dog(7); "
        "    var o: Object := d; "
        "    return (case o of Dog => 11); "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 11);
}

// ── (d) BARE imported CLASS pattern with extra arms (3-way dispatch) ────

// Three-way dispatch: `case o of Dog => 11, Cat => 33, _ => 22` where BOTH
// `Dog` and `Cat` are imported from different modules. Verifies that the
// imported-symbols widening handles MULTIPLE imported class patterns in
// one `case`, each routing through its own imported TypeSymbol and its own
// `descArray_` entry. Pre-fix: every imported name reached the #1847
// diagnostic. Post-fix: 11 (Dog wins for a Dog).
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           case_two_imported_classes_three_way_spec_19_3) {
    auto dir = wtDir2845();
    const std::string libDog =
        "module Lib2845d_dog; "
        "public class Dog "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Dog; ";
    const std::string libCat =
        "module Lib2845d_cat; "
        "public class Cat "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Cat; ";
    // Build both libs into the same dir; pass BOTH as imports.
    std::string stemDog = unique_stem("bezel_2845d_libDog_");
    std::string stemCat = unique_stem("bezel_2845d_libCat_");
    auto dogSrc = std::make_shared<TempFile>(
        (dir / (stemDog + "_lib.bzl")).string());
    auto dogObj = std::make_shared<TempFile>(
        (dir / (stemDog + "_lib.o")).string());
    auto catSrc = std::make_shared<TempFile>(
        (dir / (stemCat + "_lib.bzl")).string());
    auto catObj = std::make_shared<TempFile>(
        (dir / (stemCat + "_lib.o")).string());
    writeSource(dogSrc->path, libDog);
    writeSource(catSrc->path, libCat);
    {
        bezel::Driver d1;
        if (!d1.compileObject(dogSrc->path, dogObj->path).success) {
            BEZEL_EXPECT_TRUE(false); return;
        }
    }
    {
        bezel::Driver d2;
        if (!d2.compileObject(catSrc->path, catObj->path).success) {
            BEZEL_EXPECT_TRUE(false); return;
        }
    }
    const std::string app =
        "module App2845d; "
        "import Lib2845d_dog; "
        "import Lib2845d_cat; "
        "public class MainClass "
        "  public static method Main(): int "
        "    var d: Dog := new Dog(7); "
        "    var o: Object := d; "
        "    return (case o of Dog => 11, Cat => 33, _ => 22); "
        "  end Main; "
        "end MainClass; ";
    std::string stemApp = unique_stem("bezel_2845d_app_");
    auto appSrc = std::make_shared<TempFile>(
        (dir / (stemApp + "_app.bzl")).string());
    auto appExe = std::make_shared<TempFile>(
        (dir / (stemApp + "_app")).string());
    writeSource(appSrc->path, app);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dd; dd.metadataPath = dogObj->path; dd.objectPath = dogObj->path;
    deps.push_back(dd);
    bezel::Dependency dc; dc.metadataPath = catObj->path; dc.objectPath = catObj->path;
    deps.push_back(dc);
    bezel::CompileResult rb = driver.compileFile(appSrc->path, appExe->path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(appExe->path);
    BEZEL_EXPECT_EQ(code, 11);
}

// ── (f) ANTI-REGRESSION — local-class bare-name pattern still works ──────

// Identical to (a) but `Dog` declared in the SAME module as the case. Pre-fix:
// 11 (already worked). Post-fix: 11. Pins that the widening did not break
// the local-symbol path.
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           in_module_class_bare_name_still_works_spec_19_3) {
    const std::string prog =
        "module M2845f; "
        "class Dog "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "end Dog; "
        "class MainClass public static method Main(): int "
        "  var d: Dog := new Dog(11); "
        "  var o: Object := d; "
        "  return (case o of Dog => 11, _ => 22); "
        "end Main; end MainClass;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ── (g) ANTI-REGRESSION — local-class named-const pattern still works ────

// Identical to (b) but `K` declared in the SAME module as the case. Pre-fix:
// 11. Post-fix: 11. Pins that the imported-const widening did not break the
// local-const path.
BEZEL_TEST(xmod_bare_name_pattern_over_imported_2845,
           in_module_qualified_const_still_works_spec_19_3) {
    const std::string prog =
        "module M2845g; "
        "class K "
        "  public const Limit: int := 7; "
        "end K; "
        "class MainClass public static method Main(): int "
        "  var n: int := 7; "
        "  return (case n of K.Limit => 11, _ => 22); "
        "end Main; end MainClass;";
    auto r = bezel::test::runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

}  // namespace bezel::codegen
