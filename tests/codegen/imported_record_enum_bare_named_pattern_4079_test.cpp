// Spec-derived regression coverage for issue #4079 — §19.5 / §19.3 / §2.8.3
// rule 4 (v0.294): a bare (non-destructuring) type-test pattern over an
// IMPORTED record or enum (`case p of LibPt => ...` after `import Lib`
// declaring `record LibPt` or `enum LibColor`) was correctly ACCEPTED by
// sema (`ExprTC::checkTopLevelNamedPattern`) but SPURIOUSLY REJECTED by
// codegen's `Emitter::patternMatches` bare-`NamedPattern` record/enum loop
// (LlvmBackend_Exprs2.cpp, fix #1206 — and the `TypeAliasSymbol` arm
// #4076 added INSIDE it for the alias case). The loop walked
// `ctx_.symbols()` (module-local only, SemaContext.hpp:90 `symbols_`) and
// NEVER fell back to `ctx_.importedSymbols()`, so an imported record/enum's
// TypeSymbol — registered into `importedSymbols_` by ModuleLoader — never
// appeared there, and the loop fell through to the #1847 backstop
// diagnostic ("not supported by the backend"). The identical pattern shape
// over a CLASS/INTERFACE from the same imported module compiled and ran
// fine because the sibling `Emitter::descriptorForPatternName`
// (LlvmBackend_Setup.cpp, fix #2845) already had the two-pass widening —
// local first, then imported — that the record/enum loop lacked.
//
// This is the SIXTH sibling of the same local-only-symbol-lookup gap
// (#2844/#2845/#3197, documented in `descriptorForPatternName`'s own
// comment at LlvmBackend_Setup.cpp:5486-5501 and applied at the sibling
// sites cited there: LlvmBackend_Stmts.cpp:7657-7666,
// LlvmBackend_Assign.cpp:10008-10020); the previous five fixed the gap at
// other call sites but missed this record/enum loop. #4079 is the
// dedicated fix: add the second pass over `ctx_.importedSymbols()` to that
// loop, mirroring `descriptorForPatternName`'s shape exactly.
//
// The tests below MUST be TWO-MODULE (decl in module A, use in module B)
// per the imported-symbol axis — a local-only test would mask the real
// fix (the loop's local pass already works). The build is
// `bezelc -c` + `bezelc --import` cross-module, the same shape the issue
// repro used.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const std::filesystem::path& wtDir4079() {
    return wtScratchDir("wt-4079-codegen");
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

struct LibHandle {
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& libSrc) {
    LibHandle h;
    auto dir = wtDir4079();
    std::string stem = unique_stem("bezel_4079lib_");
    std::string srcPath = (dir / (stem + "_lib.bzl")).string();
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(srcPath, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(srcPath, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    bool ok = false;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& libObjPath, const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    auto dir = wtDir4079();
    std::string stem = unique_stem("bezel_4079app_");
    std::string srcPath = (dir / (stem + "_app.bzl")).string();
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(srcPath, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(srcPath, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

int runExe(const std::string& exePath) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exePath.c_str(), exePath.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}
}  // namespace

// ── T1 — #4079 issue repro, verbatim (§19.5): a bare record type-test
// pattern over an IMPORTED record must match and compute the correct value
// (7, exactly like the local-record form) — pre-fix it falls through to
// the #1847 backstop diagnostic at the `case` arm.
BEZEL_TEST(imported_record_enum_bare_named_pattern_4079,
           imported_record_bare_matches) {
    const std::string lib =
        "module Lib4079; "
        "public record LibPt4079(x: int, y: int); ";
    const std::string app =
        "module M4079; "
        "import Lib4079; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Lib4079.LibPt4079(1, 2); "
        "    return (case p of LibPt4079 => 7, _ => 0); "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    BEZEL_EXPECT_EQ(runExe(a.exePath), 7);
}

// ── T2 — #4079 issue repro, verbatim (§19.5): a bare ENUM type-test
// pattern over an IMPORTED enum must match and compute the correct value
// (42) — pre-fix the same #1847 backstop fires at the `case` arm.
BEZEL_TEST(imported_record_enum_bare_named_pattern_4079,
           imported_enum_bare_matches) {
    const std::string lib =
        "module LibEnum4079; "
        "public enum LibColor4079 "
        "  Red := 1, Green := 2, Blue := 3; "
        "end LibColor4079; ";
    const std::string app =
        "module MEnum4079; "
        "import LibEnum4079; "
        "class C "
        "  public static method Main(): int "
        "    var c: LibEnum4079.LibColor4079 := LibEnum4079.LibColor4079.Red; "
        "    return (case c of LibColor4079 => 42, _ => 0); "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    BEZEL_EXPECT_EQ(runExe(a.exePath), 42);
}

// ── T3 — Anti-regression: the LOCAL (non-imported) record path must
// still behave identically — pins that adding the imported pass did not
// disturb the pre-existing local `TypeSymbol` path fix #1206 established.
// Also doubles as a sanity check that the local-only branch the issue
// notes "was already correct" still is, against a future widening that
// over-eagerly resolves the FIRST matching symbol regardless of scope.
BEZEL_TEST(imported_record_enum_bare_named_pattern_4079,
           local_record_bare_still_correct) {
    auto r = runAot(
        "module M; "
        "public record PtLocal4079(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new PtLocal4079(1, 2); "
        "    return (case p of PtLocal4079 => 7, _ => 0); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── T4 — Anti-regression: the LOCAL enum path must still behave
// identically — pins the local `TypeSymbol` arm against a fix that
// reordered scope priority (an imported match must NOT shadow a local of
// the same name; the local `ctx_.symbols()` pass runs FIRST by design,
// mirroring `descriptorForPatternName`'s shape).
BEZEL_TEST(imported_record_enum_bare_named_pattern_4079,
           local_enum_bare_still_correct) {
    auto r = runAot(
        "module M; "
        "public enum ELocal4079 A, B, C; end ELocal4079; "
        "class C "
        "  public static method Main(): int "
        "    var e := ELocal4079.B; "
        "    return (case e of ELocal4079 => 42, _ => 0); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── T5 — Appendix C.1 / §12.7 / §19.3 (v0.313, #5238; flipped from the
// #4079-era "falls through, value 0" pin): an IMPORTED record pattern naming a
// DIFFERENT record than the concrete value scrutinee's static type can never
// match, and C.1 makes it a compile error ("the same concrete operand or
// scrutinee against a value type other than its own ... is a compile error"),
// exactly as the local form (#5188). The reject text is pinned by
// tests/sema/case_type_pattern_qualified_imported_5238_test.cpp; this row pins
// that the AOT driver rejects the program (it no longer reaches codegen).
BEZEL_TEST(imported_record_enum_bare_named_pattern_4079,
           imported_record_bare_other_record_rejected_5238) {
    const std::string lib =
        "module LibTwoRec4079; "
        "public record LibPtTwo4079(x: int, y: int); "
        "public record LibQtTwo4079(a: int); ";
    const std::string app =
        "module MTwoRec4079; "
        "import LibTwoRec4079; "
        "class C "
        "  public static method Main(): int "
        "    var p := new LibTwoRec4079.LibPtTwo4079(1, 2); "
        "    return (case p of LibQtTwo4079 => 5, _ => 0); "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_FALSE(a.ok);
}
