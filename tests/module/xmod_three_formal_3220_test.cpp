// fix #3220 (§22.4 "Inline `out` declarations" / §22.5 "Optional parameters
// and named arguments"; §5.1 implicit conversions; v0.254) — three further
// AST-only reads living in the two argument loops of
// src/sema/ExprTypecheck_Exprs.cpp, answering three different questions, each
// producing a measured wrong answer across a module boundary.
//
// #3188 and #3206 collapsed the SIX copies of the recipient-TYPE question in
// the two loops onto two accessors (`formalOutParamType2`, `formalCtorOutParamType`).
// This commit is the second pass: the same loops carry three more AST-only
// reads that answer different questions — `formalMod`, the NewExpr literal-
// widening hint, the CallExpr literal-widening hint — and each has the same
// defect shape (AST side → null → default answer → wrong verdict), with the
// OPPOSITE direction for the two widening hints (FALSE REJECT) and the SAME
// direction for `formalMod` (FALSE ACCEPT).
//
// The fix is uniform: a per-question accessor that prefers the AST declaration
// when present and falls back to the imported callee's symbol, mirroring the
// two recipient-type accessors exactly.
//
// ── THE THREE SITES ─────────────────────────────────────────────────────────
//
//   (1) `formalMod`            ExprTypecheck_Exprs.cpp :6505-6520   §22.4
//       Drives `checkRefOutOperandCategory(a, formalMod)`. The `out owned`
//       gate there rejects a non-inline recipient with the §22.3/§22.4
//       citation — but only if `formalMod == "out owned"`. For an imported
//       callee `calleeMd2` is null and `formalMod` stays empty, so the gate
//       is bypassed and `Make(out existing_local)` (a §22.4 VIOLATION) is
//       accepted.
//
//   (2) CallExpr literal-widening hint    ExprTypecheck_Exprs.cpp :6410-6422
//       #1051 / §5.1 target-types an unsuffixed integer literal argument to
//       a `uint`/`long`/`ulong` parameter. The hint is read off
//       `calleeMd2->params[pos2].type` only, which is null for an imported
//       callee — so `H.TakeU(3000000000)` is rejected ("integer literal does
//       not fit in `int`") while the byte-identical local twin accepts and
//       runs.
//
//   (3) NewExpr literal-widening hint     ExprTypecheck_Exprs.cpp :6818-6870
//       Same shape as (2) for the constructor loop. The `ctorDecl`-only read
//       is the first arm; the second arm (record positional fields) is
//       already symbol-side via #3141.
//
// ── THE THREE ACCESSORS ────────────────────────────────────────────────────
//
//   formalParamModifier2   (mirrors formalOutParamType2)   for site (1)
//   formalCtorParamType    (mirrors formalCtorOutParamType) for site (3)
//
// Site (2) reuses `formalOutParamType2` verbatim.
//
// ⚠ ITEM (1) IS A FALSE ACCEPT. Per §22.4 an `out owned` formal may only
// receive an inline `out using var` / `out owned var` declaration; the
// imported callee was silently accepting non-inline shapes. The new behaviour
// matches §22.4 and the local callee's already-rejecting behaviour.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the sanitizer-honouring compileFile
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique temp-file stem. The old std::rand()-based naming was the
// root cause of the #3002-class xmod COVERAGE SHORT: std::rand() is a single
// global PRNG shared across every test in the same shard (it is not
// thread-safe and the seed is per-process), and the birthday-paradox collision
// probability on ~186 tests per shard is ~0.8% — a collision overwrites the
// loser's source file mid-shard, garbles the in-flight compileObject child's
// read, and the shard crashes. The fix: a process-wide atomic counter
// combined with getpid() for cross-process uniqueness. O(1), no syscall per
// call beyond the once-cached getpid(), and the stem is unique across the
// whole /tmp scratch dir.
std::atomic<uint64_t> &g_temp_stem_counter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}
std::string unique_stem(const char* prefix) {
    return std::string(prefix) + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(
               g_temp_stem_counter().fetch_add(1, std::memory_order_relaxed));
}

struct TempFile3220 {
    std::string path;
    explicit TempFile3220(std::string p) : path(std::move(p)) {}
    ~TempFile3220() { std::remove(path.c_str()); }
};

void writeSource3220(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// ── the dependency for ITEMS 2 and 3 ────────────────────────────────────────
//
// `W` — a class with a `uint` constructor parameter (`W(uu: uint)`). Item 3
//        exercises `new W(3000000000)`; the ctor stores into `u` and the
//        caller compares.
// `H` — the holder class that exposes `TakeU(u: uint): uint`. Returns u.
//
// The verification pattern compares the returned uint to 3000000000U: equal →
// 1, not equal → 91. Pre-fix the literal was rejected and the test program
// did not build at all (rc = -1 from runAot); post-fix it builds and runs,
// and the assertion reads back the literal widening worked.
const char* kLibDecls3220 =
    "public class W "
    "  public u: uint; "
    "  public constructor(uu: uint) u := uu; end constructor; "
    "end W; "
    "public class H "
    "  public static method TakeU(u: uint): uint "
    "    return u; "
    "  end TakeU; "
    "end H;";

// ── the dependency for ITEM 1 (an `out owned` method, parallel to #3188) ───
//
//   `Res` — the counted resource; its destructor bumps a process-global counter.
//   `MakeLib` — `Make(out owned r: Res)` is the §22.4-style `out owned` formal.
const char* kLibDecls3220_O =
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() v := 1; end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "end Res; "
    "public class MakeLib "
    "  public static method Make(out owned r: Res): bool "
    "    r := new Res(); "
    "    return true; "
    "  end Make; "
    "end MakeLib;";

std::string libSource3220() {
    return "module WideningLib3220; " + std::string(kLibDecls3220);
}

std::string libSource3220_O() {
    return "module OutOwnLib3220; " + std::string(kLibDecls3220_O);
}

// ── two-module build + RUN. Used by ACCEPT tests. ───────────────────────────
int compileImportAndRun3220(const std::string& libSrc,
                            const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3220r");
    TempFile3220 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3220 aObj((dir / (stem + "_a.o")).string());
    writeSource3220(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile3220 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3220 bExe((dir / stem).string());
    writeSource3220(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ── two-module build, stderr captured. Used by REJECTION tests. ─────────────
struct ImportCapture3220 {
    bool dependencyCompiled = false;
    bool appCompiled = false;
    std::string error;
};

std::string captureStderrOf3220(const std::function<void()>& body) {
    std::string errPath =
        (fs::temp_directory_path() /
         (unique_stem("bz3220_err") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    body();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return err;
}

ImportCapture3220 compileImport3220(const std::string& libSrc,
                                    const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3220");
    TempFile3220 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3220 aObj((dir / (stem + "_a.o")).string());
    writeSource3220(aSrc.path, libSrc);
    ImportCapture3220 out;
    {
        bezel::Driver driver;
        out.dependencyCompiled = driver.compileObject(aSrc.path, aObj.path).success &&
                                 std::ifstream(aObj.path).good();
    }
    if (!out.dependencyCompiled) return out;
    TempFile3220 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3220 bExe((dir / stem).string());
    writeSource3220(bSrc.path, appSrc);
    out.error = captureStderrOf3220([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        out.appCompiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                            opts).success;
    });
    return out;
}

// ── single-module compile + RUN for ACCEPT twin. ────────────────────────────
int compileLocalAndRun3220(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3220lr");
    TempFile3220 srcFile((dir / (stem + ".bzl")).string());
    TempFile3220 exeFile((dir / stem).string());
    writeSource3220(srcFile.path, src);
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    if (!bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {},
                                         opts)
             .success)
        return -1;
    int status = std::system(exeFile.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ── single-module compile, stderr captured, for REJECTION twin. ─────────────
struct LocalCapture3220 {
    bool compiled = false;
    std::string error;
};

LocalCapture3220 compileLocal3220(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3220l");
    TempFile3220 srcFile((dir / (stem + ".bzl")).string());
    TempFile3220 exeFile((dir / stem).string());
    writeSource3220(srcFile.path, src);
    LocalCapture3220 out;
    out.error = captureStderrOf3220([&] {
        bezel::Driver driver;
        codegen::CodegenOptions opts;
        out.compiled = bezel::test::compileFileForTest(driver, srcFile.path,
                                                       exeFile.path, {}, opts)
                          .success;
    });
    return out;
}

bool mentions3220(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// ITEM 1 — `formalMod` §22.4 FALSE ACCEPT.
//
// Pre-fix: an imported `Make(out owned r)` accepts a non-inline `out some_local`
//          (the rejection gate in `checkRefOutOperandCategory` runs only when
//          `formalMod == "out owned"`, and for an imported callee the AST
//          `calleeMd2` is null so `formalMod` stayed empty).
// Post-fix: rejected with the §22.3/§22.4 "must be an inline `out var x`
//          or `out using var x`" diagnostic — reworded at fix #3842, which
//          retired the `out owned var x` spelling per §A.4 / v0.289 L2.
// ===========================================================================
BEZEL_TEST(xmod_three_formal_3220,
           item1_imported_out_owned_formalMod_rejects_non_inline_22_4) {
    auto r = compileImport3220(
        libSource3220_O(),
        "module OutOwnApp3220; import OutOwnLib3220; "
        "public class C "
        "  public static method Main(): int "
        "    var preexisting := new Res(); "
        "    var ok := MakeLib.Make(out preexisting); "  // NOT inline — must reject
        "    if ok then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(mentions3220(r.error, "must be an inline"));
    // The citation gained ", v0.289 L2" at fix #3842, which reworded the
    // remedy to name the two admitted inline forms (`out var x` / `out using
    // var x`) after `out owned var x` was retired per §A.4.
    BEZEL_EXPECT_TRUE(mentions3220(r.error, "(§22.3 / §22.4, v0.289 L2)"));
}

// T1b — the local twin of T1. Rejects before the fix and must keep rejecting.
BEZEL_TEST(xmod_three_formal_3220,
           item1_local_twin_out_owned_formalMod_rejects_non_inline_22_4) {
    auto r = compileLocal3220(
        "module OutOwnCtl3220; " + std::string(kLibDecls3220_O) +
        "public class C "
        "  public static method Main(): int "
        "    var preexisting := new Res(); "
        "    var ok := MakeLib.Make(out preexisting); "
        "    if ok then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.compiled);
    BEZEL_EXPECT_TRUE(mentions3220(r.error, "must be an inline"));
    // The citation gained ", v0.289 L2" at fix #3842, which reworded the
    // remedy to name the two admitted inline forms (`out var x` / `out using
    // var x`) after `out owned var x` was retired per §A.4.
    BEZEL_EXPECT_TRUE(mentions3220(r.error, "(§22.3 / §22.4, v0.289 L2)"));
}

// T1c — over-widening control: the accessor's local arm must be UNCHANGED.
// An inline `out using var` recipient against an `out owned` parameter must
// keep accepting and the program must return 1.
BEZEL_TEST(xmod_three_formal_3220,
           item1_local_out_owned_inline_recipient_is_accepted_22_4) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3220(
        "module OutOwnCtl3220c; " + std::string(kLibDecls3220_O) +
        "public class C "
        "  public static method Main(): int "
        "    var ok := MakeLib.Make(out using var made); "
        "    if not ok then return 0; end; "
        "    return made.v; "
        "  end Main; "
        "end C;"), 1);
}

// ===========================================================================
// ITEM 2 — CallExpr literal-widening hint FALSE REJECT.
//
// `H.TakeU(3000000000)` with a `uint` parameter. TakeU returns the parameter
// unchanged. Pre-fix the widening hint was AST-only and answered None for an
// imported callee, so the literal was rejected ("does not fit in `int`").
// Post-fix: imported accepts; the program returns 1 if the value round-trips.
// ===========================================================================
BEZEL_TEST(xmod_three_formal_3220,
           item2_imported_call_uint_literal_widens_5_1) {
    BEZEL_EXPECT_EQ(compileImportAndRun3220(
        libSource3220(),
        "module CallWidenApp3220; import WideningLib3220; "
        "public class C "
        "  public static method Main(): int "
        "    var got := H.TakeU(3000000000); "
        "    if got = 3000000000U then return 1; end if; "
        "    return 91; "
        "  end Main; "
        "end C;"), 1);
}

// T2b — the local twin of T2.
BEZEL_TEST(xmod_three_formal_3220,
           item2_local_call_uint_literal_widens_control_5_1) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3220(
        "module CallWidenCtl3220; " + std::string(kLibDecls3220) +
        "public class C "
        "  public static method Main(): int "
        "    var got := H.TakeU(3000000000); "
        "    if got = 3000000000U then return 1; end if; "
        "    return 91; "
        "  end Main; "
        "end C;"), 1);
}

// T2c — anti-vacuity: a too-large literal for an `int` parameter (declared
// in the same imported library) MUST keep rejecting. Pins that the widening
// is gated on the parameter's declared type.
BEZEL_TEST(xmod_three_formal_3220,
           item2c_imported_call_int_too_large_still_rejected_5_1) {
    auto r = compileImport3220(
        libSource3220(),
        "module CallNoWidenApp3220; import WideningLib3220; "
        "public class W2 "
        "  public static method TakeI(i: int): int "
        "    return i; "
        "  end TakeI; "
        "end W2; "
        "public class C "
        "  public static method Main(): int "
        "    return W2.TakeI(3000000000); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(mentions3220(r.error, "does not fit in `int`"));
}

// ===========================================================================
// ITEM 3 — NewExpr literal-widening hint FALSE REJECT.
//
// `new W(3000000000)` for an imported `class W(uu: uint)`. The constructor
// loop's first arm read `ctorDecl->params[pos].type` only, which is null for
// an imported constructor — so the widening hint was never derived and the
// literal was rejected. Post-fix: imported accepts and stores the value;
// the verification reads `w.u` and returns 1.
// ===========================================================================
BEZEL_TEST(xmod_three_formal_3220,
           item3_imported_ctor_uint_literal_widens_5_1) {
    BEZEL_EXPECT_EQ(compileImportAndRun3220(
        libSource3220(),
        "module NewWidenApp3220; import WideningLib3220; "
        "public class C "
        "  public static method Main(): int "
        "    var w := new W(3000000000); "
        "    if w.u = 3000000000U then return 1; end if; "
        "    return 91; "
        "  end Main; "
        "end C;"), 1);
}

// T3b — the local twin of T3.
BEZEL_TEST(xmod_three_formal_3220,
           item3_local_ctor_uint_literal_widens_control_5_1) {
    BEZEL_EXPECT_EQ(compileLocalAndRun3220(
        "module NewWidenCtl3220; " + std::string(kLibDecls3220) +
        "public class C "
        "  public static method Main(): int "
        "    var w := new W(3000000000); "
        "    if w.u = 3000000000U then return 1; end if; "
        "    return 91; "
        "  end Main; "
        "end C;"), 1);
}

}  // namespace bezel::module