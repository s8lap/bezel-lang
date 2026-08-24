// Spec-derived regression test for issue #3230 — family M11 ("[K]int over an
// IMPORTED const never folds") of the imported-symbol classification seam
// (umbrella #3152). §17.14 "Constant expressions" / §15.1 "Inline arrays" /
// §22.12 "Static members"; spec v0.254.
//
// ── MEASURED, on 88c8f93 (HEAD), with a real two-module build ───────────────
//
// An imported class-level `const` used as an inline-array SIZE leaves
// `arraySizeValue` at -1 because the fold closures walk `cms->decl->value`,
// which is null for an imported member (the symbol is reconstructed from
// dependency metadata, fix #328). The fold silently returns nullopt, codegen
// falls back to the scalar default, and an indexing expression against the
// resulting malformed element GEP degrades to a zero/null return value (the
// #2491 "return-value expression failed to lower" diagnostic) — silently
// producing the wrong exit code.
//
// Repro shape: a `Der` class inheriting from an imported base that declares
// `public const K: int := 41;`. The `§2.7 rule 1` bare-identifier path
// reaches `K` from inside `Der`, and the fold closure walks `currentOuterType_`
// and its class-base chain — exactly the shape that should make `[K]int`
// fold to `[41]int`. Pre-fix it does not; `a[0] := 100; return a[0];` exits 0.
//
// ── THE fix ────────────────────────────────────────────────────────────────
//
// `ConstMemberSymbol` carries a typed integer value (`intValue`,
// `std::optional<std::int64_t>`) populated for LOCAL consts from the existing
// AST path's fold result (sema's `foldIntConstExpr` is the same shape the
// four sites already walk) and for IMPORTED consts by
// `ModuleLoader::convertMember` (parsing the textual `MetaMember::valueText`
// — the same wire text the existing `importedConstValue` channel already
// carries, fix #2898's "FOLD, don't render"). The four fold closures
// (TypeResolution_Names's `foldIntConstExpr` and
// `findConstShiftDomainFaultInScope`, TypeResolution_Checks's
// `checkTypeConsts`'s sibling-const walker, and ExprTypecheck's
// `foldIntConstExprTC`) fall back to `cms->intValue` when the AST walk
// yields no value (no `cms->decl` for imports, or `cd->value == nullptr`).
//
// The wire format is unchanged — no schema bump, no new field. Parsing the
// existing textual value at load time is cheaper than threading the integer
// across as a new conditional record and gives the four sites a single
// authority (the symbol-side value) instead of two (AST decl + the textual
// channel). Non-integer consts (string / bool / char / float / decimal)
// leave `intValue` empty and continue to fold through codegen's existing
// `foldImportedConstText` path unchanged.
//
// ── WHAT THESE ROWS PIN ────────────────────────────────────────────────────
//
// Every row is an imported/local twin over character-identical declarations,
// so a row can only pass by the two sides agreeing. The rows cover the
// reachable fold sites the issue names — `[K]int` array size (§15.1) and
// `var b: byte := K;` (§5.1 constant narrowing). Over-rejection control: an
// imported string const (T3) must continue to read its value through the
// existing `foldImportedConstText` path.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const std::filesystem::path& wtDir3230() {
    return wtScratchDir("wt-3230");
}

struct TempFile3230 {
    std::string path;
    explicit TempFile3230(std::string p) : path(std::move(p)) {}
    ~TempFile3230() { std::remove(path.c_str()); }
};

void writeSource3230(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3230(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int runCapture3230(const std::string& exe) {
    std::string runCmd = exe + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    if (!p) return -2;
    char buf[256];
    std::string out;
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1)             return -2;
    else if (WIFSIGNALED(status)) return -3;
    else if (!WIFEXITED(status))  return -1;
    return WEXITSTATUS(status);
}

// Returns the importer's exit status, -1 on a build/run failure, or -2 when
// the dependency itself could not be produced (no toolchain).
int compileImportAndRun3230(const std::string& libSrc,
                            const std::string& appSrc) {
    const auto& dir = wtDir3230();
    std::string stem = unique_stem("bz3230_");
    TempFile3230 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3230 aObj((dir / (stem + "_a.o")).string());
    TempFile3230 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3230 bExe((dir / stem).string());

    writeSource3230(aSrc.path, libSrc);
    if (!compileToObj3230(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    writeSource3230(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    if (!std::ifstream(bExe.path).good()) return -1;
    return runCapture3230(bExe.path);
}

int compileAndRun3230(const std::string& src) {
    const auto& dir = wtDir3230();
    std::string stem = unique_stem("bz3230L_");
    TempFile3230 sSrc((dir / (stem + ".bzl")).string());
    TempFile3230 sExe((dir / stem).string());
    writeSource3230(sSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sSrc.path, sExe.path).success)
        return -1;
    if (!std::ifstream(sExe.path).good()) return -1;
    return runCapture3230(sExe.path);
}

// Shared declarations — verbatim across the dependency module and the
// single-module twin.
const char* kDecls3230 =
    "public class BaseArr "
    "  public const K: int := 41; "
    "  public constructor() end constructor; "
    "end BaseArr; "
    "public class BaseByte "
    "  public const K: int := 41; "
    "  public constructor() end constructor; "
    "end BaseByte; "
    "public class BaseStr "
    "  public const S: string := \"hello\"; "
    "  public constructor() end constructor; "
    "end BaseStr;";

std::string lib3230(const std::string& tag) {
    return std::string("module Lib3230" + tag + "; ") + kDecls3230;
}

// The importer's body — Main lives inside the derived class so the §2.7
// rule-1 bare-name path sees `K` / `S` through the inherited base.
std::string app3230Arr(const std::string& tag) {
    return "module App3230" + tag + "; import Lib3230" + tag + "; "
           "public class C" + tag + ": BaseArr "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var a: [K]int; "
           "    a[0] := 100; "
           "    return a[0]; "
           "  end Main; "
           "end C" + tag + ";";
}
std::string app3230Byte(const std::string& tag) {
    return "module App3230" + tag + "; import Lib3230" + tag + "; "
           "public class C" + tag + ": BaseByte "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var b: byte := K; "
           "    return int(b); "
           "  end Main; "
           "end C" + tag + ";";
}
std::string app3230Str(const std::string& tag) {
    return "module App3230" + tag + "; import Lib3230" + tag + "; "
           "public class C" + tag + ": BaseStr "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var s: string := S; "
           "    return s.Length; "
           "  end Main; "
           "end C" + tag + ";";
}
std::string twin3230Arr(const std::string& tag) {
    return "module Twin3230" + tag + "; " + kDecls3230 +
           " public class C" + tag + ": BaseArr "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var a: [K]int; "
           "    a[0] := 100; "
           "    return a[0]; "
           "  end Main; "
           "end C" + tag + ";";
}
std::string twin3230Byte(const std::string& tag) {
    return "module Twin3230" + tag + "; " + kDecls3230 +
           " public class C" + tag + ": BaseByte "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var b: byte := K; "
           "    return int(b); "
           "  end Main; "
           "end C" + tag + ";";
}
std::string twin3230Str(const std::string& tag) {
    return "module Twin3230" + tag + "; " + kDecls3230 +
           " public class C" + tag + ": BaseStr "
           "  public constructor() end constructor; "
           "  public static method Main(): int "
           "    var s: string := S; "
           "    return s.Length; "
           "  end Main; "
           "end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §15.1 / §17.14: an IMPORTED class-level const used as an inline-array
// SIZE. `Der` inherits `K` from the imported base and uses `[K]int`. The
// `[K]int` type must fold to `[41]int`, and `a[0]` must read back the
// assigned value.
//
// Pre-fix the array's `arraySizeValue` stayed at -1 because the fold closure
// walked `cms->decl->value` (null for an imported const) and silently
// returned nullopt. Codegen fell back to the scalar default and the
// `a[0] := 100; return a[0];` returned 0 (degraded to zero/null, #2491).
//
// Exit 100 = `a[0]` after `a[0] := 100;` on a 41-element array.
// ===========================================================================
BEZEL_TEST(xmod_imported_const_array_size_3230,
           t1_imported_const_inline_array_size_15_1) {
    int r = compileImportAndRun3230(lib3230("T1"), app3230Arr("T1"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 100);
}

BEZEL_TEST(xmod_imported_const_array_size_3230,
           t1_twin_local_const_inline_array_size_15_1) {
    int r = compileAndRun3230(twin3230Arr("T1L"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 100);
}

// ===========================================================================
// T2 — §5.1 "Constant narrowing" / §22.12: `var b: byte := K;` for an imported
// `K: int` const. The narrowing path is foldIntConstExpr's class-member walk
// (when inherited, the §2.7 rule-1 bare spelling). Pre-fix this was a false
// REJECT (per #3149 §A.5).
//
// Exit 41 = the byte-narrowed value of K (41 fits in [0, 255]).
// ===========================================================================
BEZEL_TEST(xmod_imported_const_array_size_3230,
           t2_imported_const_byte_narrowing_5_1) {
    int r = compileImportAndRun3230(lib3230("T2"), app3230Byte("T2"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 41);
}

BEZEL_TEST(xmod_imported_const_array_size_3230,
           t2_twin_local_const_byte_narrowing_5_1) {
    int r = compileAndRun3230(twin3230Byte("T2L"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 41);
}

// ===========================================================================
// T3 — Over-rejection control: an IMPORTED string const. Must continue to
// fold through the existing `foldImportedConstText` path. `intValue` is
// empty (the declared type is not an integer-family primitive) and the new
// code leaves that path completely alone.
//
// Exit 5 = "hello".Length.
// ===========================================================================
BEZEL_TEST(xmod_imported_const_array_size_3230,
           t3_imported_string_const_unchanged_22_12) {
    int r = compileImportAndRun3230(lib3230("T3"), app3230Str("T3"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

BEZEL_TEST(xmod_imported_const_array_size_3230,
           t3_twin_local_string_const_unchanged_22_12) {
    int r = compileAndRun3230(twin3230Str("T3L"));
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

}  // namespace bezel::module