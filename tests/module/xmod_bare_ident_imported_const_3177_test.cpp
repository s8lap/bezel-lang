// Spec-derived regression test for issue #3177 — family M4 ("member TYPE
// resolution at a use site") of the imported-symbol classification seam
// (umbrella #3152). §22.12 (static members / class-level `const`) / §2.7 rule 1
// (unqualified name resolution) / §2.5 + §2.8.3 rule 4 / §4.6 (string `+`),
// v0.254. Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, Appendix A.4
// row `Exprs.cpp:1215` — "the two adjacent arms both got the fallback; this one
// did not".
//
// ── MEASURED, on ec6a02f, with a real two-module build ─────────────────────
//
//   `Console.WriteLine("k=" + K.ToString())` where the const-declaring base is
//
//     LOCAL     -> compiles, prints `k=7`
//     IMPORTED  -> REJECTED: "`+` requires both operands to be `string` — the
//                  compiler does not implicitly convert other types to `string`
//                  … (§4.6)"
//
// That is a FALSE REJECT of a legal program, and the diagnostic describes
// something the user did not write: both operands ARE strings, but `K` carried
// no exprType, so `K.ToString()` never reached the primitive-`ToString` arm and
// its result was untyped.
//
// ── CAUSE ──────────────────────────────────────────────────────────────────
//
// The bare-identifier `ConstMemberSymbol` arm (added by #3079) reads
// `ast::ConstDecl` off `Symbol::decl`, which `ModuleLoader` never assigns. The
// two ARMS BESIDE IT in the same `else if` chain already had their imported
// fallback — the `FieldSymbol` one from #2532 and the `PropertySymbol` one from
// #2504 — so this is the same fix, at the third member kind, and its absence is
// visible only across a module boundary.
//
// The fallback reads `ctx_.importedMemberType(cms)` directly rather than
// `sema::storageComponentType`: a `const` is not §9.1/§9.2 storage, and that
// accessor feeds record-storage walks whose member set must not grow.

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

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L74 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3177k() {
    return wtScratchDir("wt-3177k");
}

struct TempFile3177K {
    std::string path;
    explicit TempFile3177K(std::string p) : path(std::move(p)) {}
    ~TempFile3177K() { std::remove(path.c_str()); }
};

void writeSource3177K(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3177K(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

std::string runCapture3177K(const std::string& exe, int& code) {
    std::string runCmd = exe + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (!p) { code = -2; return out; }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1)             code = -2;
    else if (WIFSIGNALED(status)) code = -3;
    else if (!WIFEXITED(status))  code = -1;
    else                          code = WEXITSTATUS(status);
    return out;
}

// {exitCode, stdout}. -2 = dependency/toolchain unavailable (SKIP, and the
// ANTI-VACUITY guard); -1 = the importer failed to compile — which is exactly
// the pre-fix behaviour this test pins, so it must be distinguishable from a
// pass.
std::pair<int, std::string> compileImportRun3177K(const std::string& libSrc,
                                                  const std::string& appSrc) {
    auto dir = wtDir3177k();
    std::string stem = unique_stem("bz3177k_");
    TempFile3177K aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3177K aObj((dir / (stem + "_a.o")).string());
    writeSource3177K(aSrc.path, libSrc);
    if (!compileToObj3177K(aSrc.path, aObj.path)) return {-2, ""};
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3177K bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3177K bExe((dir / stem).string());
    writeSource3177K(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3177K(bExe.path, code);
    return {code, out};
}

std::pair<int, std::string> compileLocalRun3177K(const std::string& src) {
    auto dir = wtDir3177k();
    std::string stem = unique_stem("bz3177k_loc_");
    TempFile3177K sFile((dir / (stem + ".bzl")).string());
    TempFile3177K eFile((dir / stem).string());
    writeSource3177K(sFile.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sFile.path, eFile.path, {})
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3177K(eFile.path, code);
    return {code, out};
}

const char* kLib3177K =
    "module DepConst3177; "
    "public class BaseK3177 "
    "  public const K: int := 7; "
    "  public constructor() end constructor; "
    "end BaseK3177;";

}  // namespace

// ===========================================================================
// ★ T1 — FAILS WITHOUT THE FIX (by failing to COMPILE). A §22.12 class-level
// const INHERITED from an IMPORTED base, reached by the §2.7 rule-1 bare
// spelling, must carry its declared type so `K.ToString()` resolves.
// // per §22.12 / §2.7 / §2.5 / §2.8.3 rule 4 / §4.6, v0.254
// ===========================================================================
BEZEL_TEST(xmod_bare_ident_imported_const_3177,
           bare_const_inherited_from_imported_base_is_typed_22_12) {
    auto [code, out] = compileImportRun3177K(
        kLib3177K,
        "module AppConst3177; import DepConst3177; "
        "public class DerK3177: BaseK3177 "
        "  public constructor() end constructor; "
        "  public method Show(): int "
        "    Console.WriteLine(\"k=\" + K.ToString()); "
        "    return 0; "
        "  end Show; "
        "end DerK3177; "
        "public class CK3177 "
        "  public static method Main(): int "
        "    using var d := new DerK3177(); "
        "    return d.Show(); "
        "  end Main; "
        "end CK3177;");
    BEZEL_EXPECT_TRUE(code != -2);  // anti-vacuity: the dependency compiled
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);       // -1 pre-fix: the importer was REJECTED
    BEZEL_EXPECT_EQ(out, std::string("k=7\n"));
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL. The identical program with the base in the
// SAME module. It compiled before the fix and must still compile: it pins that
// the module boundary was the entire difference. // per §22.12 / §2.7
// ===========================================================================
BEZEL_TEST(xmod_bare_ident_imported_const_3177,
           bare_const_local_base_unchanged_control_22_12) {
    auto [code, out] = compileLocalRun3177K(
        "module LocalConst3177; "
        "public class BaseK3177L "
        "  public const K: int := 7; "
        "  public constructor() end constructor; "
        "end BaseK3177L; "
        "public class DerK3177L: BaseK3177L "
        "  public constructor() end constructor; "
        "  public method Show(): int "
        "    Console.WriteLine(\"k=\" + K.ToString()); "
        "    return 0; "
        "  end Show; "
        "end DerK3177L; "
        "public class CK3177L "
        "  public static method Main(): int "
        "    using var d := new DerK3177L(); "
        "    return d.Show(); "
        "  end Main; "
        "end CK3177L;");
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("k=7\n"));
}

}  // namespace bezel::module
