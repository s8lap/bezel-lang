// Spec-derived regression test for issue #5035 — a PUBLIC generic method
// whose ONLY type-parameter bound is a synthesized BUILTIN interface
// (`IEquatable<T>` / `IComparable<T>`) computed the WRONG value when called
// across a module boundary, with NO diagnostic. `EqC5035(5, 5)` (equal
// arguments, `a.Equals(b)` true) returned 72 cross-module and 71 in the
// single-module twin.
//
// spec §14.1 "Generic methods" (a generic method's body is monomorphized per
// instantiation) / §2.8.3 rule 2 (an imported artifact must not alter an
// instantiation's semantics) / stdlib §2.8 `IComparable<T>` / §2.9
// `IEquatable<T>`, v0.307.
//
// ── THE MECHANISM (MEASURED) ────────────────────────────────────────────────
//
// fix #5009 fix-forward 4's own comment records that shipping a generic
// method's SOURCE across the module boundary is deliberately disabled when
// the only bound is a synthesized builtin interface (the importer's re-parse
// context would resynthesize `IEquatable`/`IComparable`/`INumber` as a
// DIFFERENT symbol and the §14.2 call-site check would then reject a
// conforming argument) — see `xmod_constraint_dispatch_generic_method_5009
// _test.cpp` T9. So a call to such a method ALWAYS runs the EXPORTER's own
// i32-placeholder-compiled body (#2821): `T`'s LLVM representation there is a
// bare `i32` value, never a pointer.
//
// The placeholder body's `a.Equals(b)` / `a.CompareTo(b)` call lowers through
// a fast path that requires the receiver/argument's SEMA type to already be
// resolved to a concrete `Primitive` (the `#730` substitution, gated on a
// non-empty `genericSubstSemaTy_` map). That map is populated ONLY for a
// per-call-site monomorphization — which never happens for the placeholder,
// compiled once, generically, for cross-module linkage. So the substitution
// never fires, the receiver stays an unresolved bare `T`, the fast path is
// skipped, and the call falls through to the general interface-dispatch
// machinery, which requires a POINTER receiver (`emitMethodCall`'s
// "§14.2" guard) — the placeholder's `a: T` is an i32 VALUE, so the guard
// returns `nullptr`. `Emitter::lowerIf` (its own pre-existing `#2775`/§17
// comment) then treats that null condition as a SILENT `false`: the `if`
// UNCONDITIONALLY takes the else arm, regardless of `a` and `b`.
//
// This affects `IEquatable<T>.Equals` AND `IComparable<T>.CompareTo` equally,
// and both instance and static methods. The issue's own claim that
// `CompareTo` "works" (`MaxC(3, 44)` gives 44 both sides) is a TEST-DATA
// COINCIDENCE: `3.CompareTo(44) > 0` is false, so the always-false-branch bug
// happens to compute the mathematically correct answer for that PARTICULAR
// input. `MaxC5035(9, 3)` (T4/T6 below), where the TRUE branch is the correct
// one, exposes that `CompareTo` was equally broken.
//
// ── THE FIX ──────────────────────────────────────────────────────────────
//
// `LlvmBackend_Exprs.cpp`, `genericParamSoundAtI32ForPlaceholder`: when no
// `genericSubstSemaTy_` substitution resolves a bare type parameter of the
// method CURRENTLY being compiled, and every "type" constraint on that
// parameter is a synthesized builtin interface (no `new()`, no user-type
// bound — fix-forward 4's own "sound at i32" set), treat it as `int`
// directly. This is safe because the ONLY way the placeholder body ever
// executes is via a cross-module call that already passed the #1943/#428
// width gate (rejects any concrete type argument whose representation does
// not fit the compiled i32 placeholder — T7-T9 below) — so any caller that
// reaches this body must have an i32-representable `T`.
//
// A user-type bound (`RIShape`-style) is UNAFFECTED: that shape is
// body-shipped and re-monomorphized instead (T9's own #5009 precedent), never
// runs this placeholder, and this fix does not touch that path.
//
// ── WHAT EACH ROW PINS ───────────────────────────────────────────────────
//
//   T1 ★ THE REPRO. `EqC5035(5, 5)` (instance, IEquatable bound) on STDOUT
//        -> "71"  (pre-fix: "72")
//   T2   the false branch is still reachable: `EqC5035(5, 6)` -> 72
//   T3 ★ static variant: `EqCS5035(5, 5)` -> 171  (pre-fix: 172)
//   T4 ★ CompareTo, instance, TRUE-branch input: `MaxCI5035(9, 3)` -> 9
//        (pre-fix: 3 — the asymmetric input the issue's own MaxC test never
//        tried)
//   T5   CompareTo, instance, false-branch input: `MaxCI5035(3, 9)` -> 9
//   T6 ★ CompareTo, static, TRUE-branch input: `MaxC5035(9, 3)` -> 9
//        (pre-fix: 3)
//   T7   NO-DISCRIMINATING DOCUMENTATION of the issue's own claim:
//        `MaxC5035(3, 44)` -> 44 (false branch; correct on BOTH sides of the
//        fix, which is why the issue's own repro read as "CompareTo works")
//   T8   DIFFERENTIAL CONTROL: T1+T2+T3+T4+T5+T6+T7 in ONE module and across
//        the boundary must both equal 71+72+171+9+9+9+44 = 385
//   T9 ★ NO-REGRESSION (#1943/#428 family): a `string` type argument is still
//        REJECTED with the located width-gate diagnostic, unchanged text
//   T10 ★ NO-REGRESSION: a `long` type argument is still REJECTED
//   T11 ★ NO-REGRESSION: a record type argument is still REJECTED
//   T12  NO-REGRESSION: a user-type (`RIShape`-bound) generic method is
//        UNAFFECTED by this fix — it is body-shipped (#5009) and computes
//        correctly cross-module, matching its single-module twin
//
// Every accept row asserts the constraint method's exact VALUE.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5035 {
    std::string path;
    explicit TempFile5035(std::string p) : path(std::move(p)) {}
    ~TempFile5035() { std::remove(path.c_str()); }
};

void writeSource5035(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled5035 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

template <typename Fn>
std::string captureStderr5035(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5035_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile (only) `appSrc` against it —
// used for the width-gate REJECT rows, where nothing is ever run.
Compiled5035 compileImport5035(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5035_");
    TempFile5035 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5035 aObj((dir / (stem + "_a.o")).string());
    writeSource5035(aSrc.path, libSrc);
    Compiled5035 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile5035 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5035 bExe((dir / stem).string());
    writeSource5035(bSrc.path, appSrc);
    r.err = captureStderr5035([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
// `compileFileForTest` / `runProgramForTest` (#4532) wire this runner into
// `gate.sh --full`: under BEZEL_AOT_FORCE_SANITIZE=address the importer is
// built with generated-code ASan while the row keeps asserting its VALUE.
int buildImportAndRun5035(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5035r_");
    TempFile5035 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5035 libObjF((dir / (id + "_lib.o")).string());
    TempFile5035 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5035 mainExeF((dir / id).string());
    writeSource5035(libSrcF.path, libSrc);
    writeSource5035(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The one-module twin — no dependency anywhere.
int buildLocalAndRun5035(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5035l_");
    TempFile5035 srcF((dir / (id + ".bzl")).string());
    TempFile5035 exeF((dir / id).string());
    writeSource5035(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// T1's STDOUT variant (task requirement: assert the repro's value on stdout,
// not the process exit code). `runProgramForTest`'s own sanitizer-sweep
// redirection touches only stderr and leaves stdout inherited (see that
// header's own comment), so a plain popen capture of stdout is unaffected by
// BEZEL_AOT_FORCE_SANITIZE either way.
// ---------------------------------------------------------------------------
struct Captured5035 {
    int depOk = 1;   // 0 => dependency toolchain unavailable
    int code = -1;
    std::string out;
};

std::string trimTrailingWs5035(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

Captured5035 runCapture5035(const std::string& exePath) {
    Captured5035 c;
    std::string cmd = exePath + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return c;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) c.out.append(buf, n);
    int status = pclose(p);
    c.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    c.out = trimTrailingWs5035(c.out);
    return c;
}

Captured5035 buildImportAndCapture5035(const std::string& libSrc,
                                       const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5035c_");
    TempFile5035 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5035 libObjF((dir / (id + "_lib.o")).string());
    TempFile5035 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5035 mainExeF((dir / id).string());
    writeSource5035(libSrcF.path, libSrc);
    writeSource5035(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    Captured5035 c;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) {
        c.depOk = 0;
        return c;
    }
    if (!std::ifstream(libObjF.path).good()) {
        c.depOk = 0;
        return c;
    }

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return c;  // code stays -1
    return runCapture5035(mainExeF.path);
}

Captured5035 buildLocalAndCapture5035(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5035lc_");
    TempFile5035 srcF((dir / (id + ".bzl")).string());
    TempFile5035 exeF((dir / id).string());
    writeSource5035(srcF.path, src);
    bezel::Driver driver;
    Captured5035 c;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return c;
    return runCapture5035(exeF.path);
}

// ---------------------------------------------------------------------------
// The dependency: `RO5035` — instance and static generic methods bound ONLY
// by `IEquatable<T>` / `IComparable<T>` (synthesized builtin interfaces, per
// #5009 fix-forward 4's "sound at i32" set — no `new()`, no user-type bound).
// ---------------------------------------------------------------------------
const char* kDeclsBody5035 =
    "public class RO5035 "
    "  public constructor() end constructor; "
    "  public method EqC5035<T>(a: T, b: T): int where T: IEquatable<T> "
    "    if a.Equals(b) then return 71; end if; "
    "    return 72; "
    "  end EqC5035; "
    "  public static method EqCS5035<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 171; end if; "
    "    return 172; "
    "  end EqCS5035; "
    "  public method MaxCI5035<T>(a: T, b: T): T where T: IComparable<T> "
    "    if a.CompareTo(b) > 0 then return a; end if; "
    "    return b; "
    "  end MaxCI5035; "
    "  public static method MaxC5035<T>(a: T, b: T): T where T: "
    "IComparable<T> "
    "    if a.CompareTo(b) > 0 then return a; end if; "
    "    return b; "
    "  end MaxC5035; "
    "end RO5035; ";

std::string dep5035() {
    return std::string("module Dep5035; ") + kDeclsBody5035;
}

std::string app5035(const std::string& tag, const std::string& mainBody) {
    return "module App5035" + tag + "; import Dep5035; " +
           "class C5035 public static method Main(): int " + mainBody +
           " end Main; end C5035;";
}

std::string local5035(const std::string& tag, const std::string& mainBody) {
    return "module Loc5035" + tag + "; " + kDeclsBody5035 +
           "class C5035 public static method Main(): int " + mainBody +
           " end Main; end C5035;";
}

// T1+T2+T3+T4+T5+T6+T7 in one Main: 71+72+171+9+9+9+44 = 385. Printed on
// stdout rather than returned as the exit code: 385 > 255 would be
// `WEXITSTATUS`-masked to 129 (CLAUDE.md's "8-bit exit truncation is not a
// compiler bug" trap) — MEASURED while developing this row.
const char* kAllBody5035 =
    "using var g := new RO5035(); "
    "var s := g.EqC5035(5, 5) + g.EqC5035(5, 6) + RO5035.EqCS5035(5, 5) + "
    "g.MaxCI5035(9, 3) + g.MaxCI5035(3, 9) + RO5035.MaxC5035(9, 3) + "
    "RO5035.MaxC5035(3, 44); "
    "Console.WriteLine(s.ToString()); "
    "return 0;";

// §14.1/§2.8.3, as the #1943 codegen gate emits it (LlvmBackend_Calls2.cpp).
const char* kArgWidthReject5035 =
    "with a type argument whose representation is not the compiled `int` "
    "(i32) placeholder";

// ---------------------------------------------------------------------------
// T12's dependency: a USER-TYPE bound (`RIShape5035`), which #5009 body-ships
// instead of running the i32 placeholder — confirming this fix's scope does
// not extend to (and is not needed by) that path.
// ---------------------------------------------------------------------------
const char* kUserBoundDeclsBody5035 =
    "public interface RIShape5035 "
    "  method Area(): int; "
    "end RIShape5035; "
    "public class RSq5035: RIShape5035 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end RSq5035; "
    "public static class RG5035 "
    "  public static method NT5035<T>(v: T): int where T: RIShape5035 "
    "    return v.Area() + 100; "
    "  end NT5035; "
    "end RG5035; ";

std::string depUser5035() {
    return std::string("module DepU5035; ") + kUserBoundDeclsBody5035;
}

std::string appUser5035(const std::string& tag, const std::string& mainBody) {
    return "module AppU5035" + tag + "; import DepU5035; " +
           "class CU5035 public static method Main(): int " + mainBody +
           " end Main; end CU5035;";
}

std::string localUser5035(const std::string& tag,
                          const std::string& mainBody) {
    return "module LocU5035" + tag + "; " + kUserBoundDeclsBody5035 +
           "class CU5035 public static method Main(): int " + mainBody +
           " end Main; end CU5035;";
}

}  // namespace

// ===========================================================================
// T1 — ★ THE REPRO, on STDOUT (not the process exit code, per this issue's
// explicit test requirement). `EqC5035(5, 5)`: `a.Equals(b)` is TRUE, so the
// correct value is 71. Pre-fix the placeholder body's `if` silently took the
// else arm regardless of the arguments: exit/print 72.
// // per §14.1 / stdlib §2.9 IEquatable<T> / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, equal_arguments_prints_71_on_stdout) {
    Captured5035 c = buildImportAndCapture5035(
        dep5035(),
        app5035("T1", "using var g := new RO5035(); "
                      "Console.WriteLine(g.EqC5035(5, 5).ToString()); "
                      "return 0;"));
    if (c.depOk == 0) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_TRUE(c.code == 0);
    BEZEL_EXPECT_EQ(c.out, std::string("71"));

    Captured5035 t = buildLocalAndCapture5035(local5035(
        "T1L", "using var g := new RO5035(); "
               "Console.WriteLine(g.EqC5035(5, 5).ToString()); "
               "return 0;"));
    BEZEL_EXPECT_EQ(t.out, std::string("71"));
}

// ===========================================================================
// T2 — the else arm is still reachable with genuinely unequal arguments:
// `EqC5035(5, 6)` -> 72 on BOTH sides of the fix (this row does not
// discriminate the bug; it documents that the fix did not turn the method
// into an always-true stub).
// // per §14.1 / stdlib §2.9, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, unequal_arguments_returns_72) {
    int rc = buildImportAndRun5035(
        dep5035(),
        app5035("T2", "using var g := new RO5035(); "
                      "return g.EqC5035(5, 6);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 72);
    BEZEL_EXPECT_EQ(buildLocalAndRun5035(local5035(
                        "T2L", "using var g := new RO5035(); "
                               "return g.EqC5035(5, 6);")),
                    72);
}

// ===========================================================================
// T3 — ★ the STATIC variant of the same bound is affected identically:
// `EqCS5035(5, 5)` -> 171. Pre-fix: 172 (the placeholder-body defect is not
// specific to instance methods).
// // per §14.1 / stdlib §2.9 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, static_equal_arguments_returns_171) {
    int rc = buildImportAndRun5035(dep5035(),
                                   app5035("T3", "return RO5035.EqCS5035(5, 5);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 171);
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5035(local5035("T3L", "return RO5035.EqCS5035(5, 5);")),
        171);
}

// ===========================================================================
// T4 — ★ CompareTo, INSTANCE, the TRUE-branch input the issue's own repro
// never exercised: `MaxCI5035(9, 3)`, `9.CompareTo(3) > 0` is true, so the
// correct answer is `a` = 9. Pre-fix: 3 (the else arm, unconditionally) —
// refutes the issue's claim that CompareTo "already works" cross-module.
// // per §14.1 / stdlib §2.8 IComparable<T> / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035,
           instance_compareto_true_branch_returns_a) {
    int rc = buildImportAndRun5035(
        dep5035(),
        app5035("T4", "using var g := new RO5035(); "
                      "return g.MaxCI5035(9, 3);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 9);
    BEZEL_EXPECT_EQ(buildLocalAndRun5035(local5035(
                        "T4L", "using var g := new RO5035(); "
                               "return g.MaxCI5035(9, 3);")),
                    9);
}

// ===========================================================================
// T5 — CompareTo, instance, false-branch input: `MaxCI5035(3, 9)` -> 9 on
// both sides (documents the else arm remains correct too).
// // per §14.1 / stdlib §2.8, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035,
           instance_compareto_false_branch_returns_b) {
    int rc = buildImportAndRun5035(
        dep5035(),
        app5035("T5", "using var g := new RO5035(); "
                      "return g.MaxCI5035(3, 9);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 9);
}

// ===========================================================================
// T6 — ★ CompareTo, STATIC, the same TRUE-branch discriminating input:
// `MaxC5035(9, 3)` -> 9. Pre-fix: 3.
// // per §14.1 / stdlib §2.8 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035,
           static_compareto_true_branch_returns_a) {
    int rc = buildImportAndRun5035(dep5035(),
                                   app5035("T6", "return RO5035.MaxC5035(9, 3);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 9);
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5035(local5035("T6L", "return RO5035.MaxC5035(9, 3);")),
        9);
}

// ===========================================================================
// T7 — NO-DISCRIMINATING DOCUMENTATION of the issue's own repro input:
// `MaxC5035(3, 44)` -> 44. `3.CompareTo(44) > 0` is false, so the
// always-false-branch bug computed the mathematically correct answer for
// THIS input on both sides of the fix — this is why the issue read as
// "CompareTo already works"; T4/T6 show it did not.
// // per §14.1 / stdlib §2.8, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035,
           static_compareto_issue_own_input_returns_44) {
    int rc = buildImportAndRun5035(dep5035(),
                                   app5035("T7", "return RO5035.MaxC5035(3, 44);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 44);
}

// ===========================================================================
// T8 — DIFFERENTIAL CONTROL. T1-T7's calls combined, once across the
// boundary and once in ONE module: both must agree and both must equal the
// hand-computed 71+72+171+9+9+9+44 = 385, read off STDOUT (see kAllBody5035's
// own comment for why not the exit code).
// // per §2.8.3 rule 2 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, cross_module_matches_same_module_twin) {
    Captured5035 t = buildLocalAndCapture5035(local5035("T8L", kAllBody5035));
    BEZEL_EXPECT_EQ(t.out, std::string("385"));
    Captured5035 c = buildImportAndCapture5035(dep5035(), app5035("T8X", kAllBody5035));
    if (c.depOk == 0) return;
    BEZEL_EXPECT_EQ(c.code, 0);
    BEZEL_EXPECT_EQ(c.out, std::string("385"));
}

// ===========================================================================
// T9 — ★ NO-REGRESSION (#1943/#428 family): a `string` type argument still
// does not fit the compiled i32 placeholder's representation and must be
// REJECTED with the located diagnostic, UNCHANGED by this fix (this fix only
// changes what happens once a call is already inside the placeholder body;
// it does not touch the width gate that decides whether a call reaches it).
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, string_type_argument_still_rejected) {
    auto r = compileImport5035(
        dep5035(),
        app5035("T9", "using var g := new RO5035(); "
                      "return g.EqC5035<string>(\"a\", \"a\");"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kArgWidthReject5035) != std::string::npos);
}

// ===========================================================================
// T10 — ★ NO-REGRESSION: a `long` type argument (8-byte representation,
// still not i32) is still REJECTED.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, long_type_argument_still_rejected) {
    auto r = compileImport5035(
        dep5035(),
        app5035("T10", "using var g := new RO5035(); "
                       "return g.EqC5035<long>(5L, 5L);"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kArgWidthReject5035) != std::string::npos);
}

// ===========================================================================
// T11 — ★ NO-REGRESSION: a record type argument (records conform to
// `IEquatable<T>` implicitly, stdlib §2.9) is still REJECTED — its
// representation is not the compiled i32 placeholder either.
// // per §14.1 / §2.8.3 / #1943 / stdlib §2.9, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035, record_type_argument_still_rejected) {
    auto r = compileImport5035(
        dep5035() + "public record Rec5035(X: int); ",
        app5035("T11", "using var g := new RO5035(); "
                       "var p := new Rec5035(5); var q := new Rec5035(5); "
                       "return g.EqC5035(p, q);"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kArgWidthReject5035) != std::string::npos);
}

// ===========================================================================
// T12 — NO-REGRESSION: a USER-TYPE bound (`RIShape5035`) is a DIFFERENT
// mechanism (#5009 body-shipping) this fix does not touch. It must keep
// computing correctly cross-module: `NT5035(new RSq5035())` -> 7 + 100 = 107.
// (An INTERNAL #2491 "degraded to a zero/null return value" warning on BOTH
// sides is expected noise, unrelated to #5035: MEASURED that the identical
// #5009-shaped dependency alone — `G5009s.Gen5009s<T>(v: T): int where T:
// IShape5009` compiled standalone with `bezelc -c` — emits the same warning
// at its own placeholder compile in the defining module, regardless of this
// fix or of cross-module import; the warning does not affect the VALUE
// either row asserts.)
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(imported_iequatable_bound_5035,
           user_type_bound_method_unaffected_by_this_fix) {
    int rc = buildImportAndRun5035(
        depUser5035(),
        appUser5035("T12", "using var s := new RSq5035(); "
                           "return RG5035.NT5035(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5035(localUser5035(
            "T12L", "using var s := new RSq5035(); "
                    "return RG5035.NT5035(s);")),
        107);
}

}  // namespace bezel::module
