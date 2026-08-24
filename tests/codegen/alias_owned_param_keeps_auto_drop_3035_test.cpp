// codegen/ownership — fix #3035 (§28.3 / §28.10, v0.240): the umbrella
// issue's REGRESSION COVERAGE EXTENSION. The narrow commit `760b33f` shipped
// the normal-exit half (move the `unwindIneligible_` bail below the
// `autoDropAtScopeEnd` branch) and a paired `owned_param_alias_autodrop_3035`
// test for the plain-class shape. #3039 (`0cb5ad4`) shipped the exception-edge
// half and a paired `owned_param_alias_unwind_disposal_3039` test. This file
// closes the remaining coverage gaps the umbrella issue names in its title —
// "for ANY type" — so the §28.3 / §28.10 invariant survives in every axis the
// issue's "for ANY type" claim ranges over:
//
//   1. ISSUE REPRO VERBATIM — the headline shape from the issue body:
//      `method F(p: owned R) { var alias := p; return alias.V; }` — single-
//      module, single-class. Pins the normal-exit auto-drop on the alias
//      path. FAIL-WITHOUT: R._n == 0 + 16-byte LSan leak from `main`.
//      PASS-WITH: R._n == 1 (W7L2 #3035 fix path).
//
//   2. CONTROL (no alias) — same method without the alias line:
//      `method F(p: owned R) { return p.V; }` — the pre-#3035 baseline. R._n
//      must be 1 both before AND after the fix; this case exists as a
//      regression guard so a future widening (e.g. widening `unwindIneligible_`
//      bail clearance beyond `autoDropAtScopeEnd`) cannot silently drop
//      the pre-existing dispose.
//
//   3. GENERIC (§14.1) — the umbrella title's "for ANY type" demands the
//      fix works for a type-parameter T too. `method F<T>(p: owned T) where
//      T: class { var alias := p; return alias.V; }`. R._n must be 1. The
//      generic-specialization path (buildSpecializedMethodFn, §14.1 / §14.4)
//      is the one where the disposal-registration code runs against a bare
//      type parameter — the #1483 fix made this reachable at all; the #3035
//      fix made the alias binding survive the walker's taint. FAIL-WITHOUT:
//      R._n == 0 + LSan leak. PASS-WITH: R._n == 1.
//
//   4. CROSS-MODULE (§2.8.3 rule 4) — the param's TYPE is declared in
//      module A and consumed via alias in module B. The dispose count must
//      still be 1 — the imported symbol's classification flows through the
//      dispose registration identically. This is the "for ANY type" axis
//      the umbrella calls out: the fix is per-symbol, not per-module, so the
//      xmod shape must be checked. R._n must be 1.
//
// FAIL-WITHOUT PROOF (case 1, the headline):
//   - Revert the W7L2 #3035 fix (`760b33f`) — restore the early
//     `unwindIneligible_` bail at the top of `registerUnwindOwnedParam`
//     (src/codegen/LlvmBackend_Setup.cpp:15117).
//   - case 1 then exits with R._n == 0; the LSan leg of `runAotChecked`
//     reports a 16-byte leak from `main` (the `r` parameter's allocation,
//     pre-fix 40-byte measured leak from issue body).
//   - With the fix, R._n == 1 and LSan is clean.
//   - `runAotChecked` returns ran=false on any LSan report, so a leak fails
//     both the count assertion AND the LSan check on the same run.
//
// SOUNDNESS COVERAGE — every cell asserts exactly-once disposal:
//   - R._n == 1 (not 0 = leak, not 2 = double-free).
//   - The control (case 2) is the "must-not-regress" pin: it was clean before
//     #3035 and must stay clean after.
//   - The generic cell (case 3) exercises the type-parameter disposal path
//     independently of the concrete-class path (case 1) — a divergence here
//     would mean a future bug-fix lands in one and breaks the other.
//   - The xmod cell (case 4) exercises the imported-symbol disposal path
//     independently of the local-symbol path — same divergence hazard.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

using bezel::test::runAotChecked;

// runAotChecked — single-module pipeline. The `r.ran ? r.exitCode : -999`
// idiom returns the dispose counter on success and -999 on a leak (LSan
// report flips `ran` to false), so a leak fails both the count assertion
// AND the LSan check on the same run.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::cout << "  diag: " << d.message << "\n";
        }
        return -999;
    }
    return r.exitCode;
}

// A dispose-counting class R (static counter `R._n` is bumped in the
// destructor; instance field `V` is what the alias reads). Mirrors the
// `owned_param_alias_autodrop_3035_test.cpp` setup.
const char* kR =
    "class R "
    "  public V: int := 0; "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; ";

std::string prog(const std::string& classesAfterR,
                 const std::string& mainBody) {
    return std::string("module M; ") + kR + classesAfterR +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// --- Cross-module harness (case 4) ------------------------------------------
//
// The xmod case uses Driver::compileObject + Driver::compileFile with a
// Dependency — the same shape #3082 / #3228 / #2844 / #2845 use for their
// import-side oracle. Per-process unique scratch dir under
// /var/tmp/bezel-scratch/ so concurrent fork'd shards do not race on the
// lib.o (same rationale as #3002).

const std::filesystem::path& wtDir3035() {
    static const std::filesystem::path dir =
        std::filesystem::path("/var/tmp/bezel-scratch") /
        unique_stem("wt-3035-3035");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
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

int runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib3035(const std::filesystem::path& dir,
                       const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_3035lib_");
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

InProcessApp buildApp3035(const std::filesystem::path& dir,
                          const std::string& libObjPath,
                          const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_3035app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath   = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

}  // namespace

// ===========================================================================
// Case 1 — fail-without proof (the umbrella #3035 HEADLINE): the issue's
// verbatim repro — a plain `var alias := r;` over an `owned` parameter `r`,
// used via `alias.V` — must still auto-drop `r` on the normal exit. Before
// the fix R._n stayed 0 (leak). After the fix R._n == 1.
// ===========================================================================
BEZEL_TEST(alias_owned_param_keeps_auto_drop_3035,
           headline_normal_exit_alias_auto_drops_param_28_3) {
    const std::string src = prog(
        "class Q "
        "  public static method F(p: owned R): int "
        "    var alias := p; "
        "    return alias.V; "       // alias.V is borrowed here (K3.13)
        "  end F; "
        "end Q; ",
        "  do Q.F(new R()); end; "
        "  return R._n; ");          // 1: callee auto-dropped the owned param once
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 2 — CONTROL (regression guard): the SAME method WITHOUT the alias line
// — the pre-#3035 baseline. R._n must be 1 both before AND after the fix;
// this case exists as a regression guard so a future widening (e.g. widening
// `unwindIneligible_` bail clearance beyond `autoDropAtScopeEnd`) cannot
// silently drop the pre-existing dispose. Before #3035 the control was 1
// (the auto-drop fires because `p.V` is a member access — a borrow read —
// so `p` stays owned and is auto-dropped at method end). After #3035 it
// stays 1 — the fix's bail-move does not touch the no-alias path.
// ===========================================================================
BEZEL_TEST(alias_owned_param_keeps_auto_drop_3035,
           control_no_alias_baseline_still_auto_drops_28_3) {
    const std::string src = prog(
        "class Q "
        "  public static method F(p: owned R): int "
        "    return p.V; "            // direct member access; borrow read of `p`
        "  end F; "
        "end Q; ",
        "  do Q.F(new R()); end; "
        "  return R._n; ");           // 1: pre-fix baseline; post-fix unchanged
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 3 — GENERIC (§14.1): the umbrella title's "for ANY type" demands the
// fix works for a type-parameter T. The generic-specialization path
// (buildSpecializedMethodFn) is the one where the disposal-registration
// code runs against a bare type parameter — the #1483 fix made this
// reachable at all; the #3035 fix made the alias binding survive the
// walker's taint. R._n must be 1. Before the fix R._n stayed 0 + LSan
// flagged a leak from `main` (the `p` parameter's allocation).
// ===========================================================================
BEZEL_TEST(alias_owned_param_keeps_auto_drop_3035,
           generic_owned_param_alias_auto_drops_14_1_28_3) {
    const std::string src = prog(
        "class Q "
        "  public static method F<T>(p: owned T): int where T: class "
        "    var alias := p; "
        "    return alias.V; "        // alias.V — borrow read of type-param
        "  end F; "
        "end Q; ",
        "  do Q.F<R>(new R()); end; "
        "  return R._n; ");           // 1: generic specialized body auto-drops
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ===========================================================================
// Case 4 — CROSS-MODULE (§2.8.3 rule 4): the param's TYPE `R` is declared in
// module A; the alias-binding call site lives in module B. This pins that
// the fix is per-symbol, not per-module — the imported symbol's classification
// flows through the dispose registration identically. R._n must be 1. This
// is the "for ANY type" axis the umbrella calls out.
// ===========================================================================
BEZEL_TEST(alias_owned_param_keeps_auto_drop_3035,
           xmod_imported_owned_param_alias_auto_drops_2_8_3_28_3) {
    auto dir = wtDir3035();
    const std::string lib =
        "module Lib3035a; "
        "public class R "
        "  public V: int := 0; "
        "  public static _n: int := 0; "
        "  destructor R._n := R._n + 1; end destructor; "
        "end R; ";
    const std::string app =
        "module App3035a; "
        "import Lib3035a; "
        "public class Q "
        "  public static method F(p: owned R): int "
        "    var alias := p; "
        "    return alias.V; "
        "  end F; "
        "end Q; "
        "public class MainClass "
        "  public static method Main(): int "
        "    do Q.F(new R()); end; "
        "    return R._n; "
        "  end Main; "
        "end MainClass; ";
    LibHandle libH = buildLib3035(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp appH = buildApp3035(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    int code = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(code, 1);    // 1: xmod alias binding still auto-drops
}

}  // namespace bezel::codegen