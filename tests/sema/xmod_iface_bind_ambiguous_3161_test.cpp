// Spec-derived regression test for issue #3161 — residue of #3143.
//
// §19.3/§19.5/§5.1/§12.9 (§2.8.3 rule 4 for the cross-module side),
// v0.254. Audit: docs/proposals/imported-symbol-audit-2026-08-13.md,
// family F6 + the §19.3 admit-residue of #3161.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// #3143 changed the variance fetch (`hasVariantGenericParam`) to consult
// the TYPE SYMBOL rather than the AST (`itd`), lifting the imported-symbol
// blanket `!itd -> accept` for that predicate. It noted but did not fix
// the same deficiency for `ifaceClashProtectedAllParams` — the predicate
// that admits a non-sealed class pattern's erased-argument arm only when
// the interface is CLASH-PROTECTED (every generic param pinned in a
// result-position member whose (indexer) parameters mention no generic
// param — so any two implementations differ in a substituting member's
// signature and §12.2's hiding clash rejects the duplicate at its own
// declaration).
//
// Pre-#3161: the pre-fix code kept a `if (!itd) return IfaceBindReject::None`
// escape hatch around the call — `ifaceClashProtectedAllParams` walks
// `itd->members` (each member's `returnType`/`params`/`indexerParams`
// AST nodes), which an IMPORTED interface does not have. The hatch meant an
// imported `IFoo<T>` with NO clash-protection (no member that pins T) was
// unconditionally admitted on the erased-argument arm — exactly the case
// where §19.3 requires the rejection.
//
// Post-#3161: `ifaceClashProtectedAllParams` walks the interface's
// `TypeSymbol::genericParams` / `TypeSymbol::members` (a `MethodSymbol` /
// `PropertySymbol` reconstructed by `ModuleLoader::convertMember`) with
// return/property/indexer-param types from `ctx.importedReturnType` /
// `ctx.importedMemberType` / `ParameterSymbol::resolvedDeclType` — the same
// shape the AST path used to read via `ctx.resolvedType`. Imported and
// local interfaces therefore reach the SAME verdict.
//
// ── MEASURED ON THE PARENT COMMIT (pre-#3161), real two-module builds
// ── (`bezelc -c lib.bzl -o lib.o`, then `bezelc --import lib.o app.bzl`)
//
//   T1  imported invariant `interface INoPin<T> end INoPin` (no member
//       pins T), `class C<T>: implements INoPin<T>` (NON-sealed, single
//       parametric conformance), `case s of var x: C<(int)->int> => …
//       end case` against scrutinee `INoPin<(int)->int>`
//         -> COMPILED CLEAN pre-#3161 (the wrong-acceptance — the (B)
//            ERASED branch's DISTINCT PIN does not fire because T is
//            pinned to the pattern arg `(int)->int` (equal to itself),
//            uniqueConformer = true; the §19.3 admit then went through
//            the pre-#3161 `!itd -> None` escape hatch without asking
//            `ifaceClashProtectedAllParams`, and the admit landed.
//            The defect is exactly §2.8.3 rule 4 violation: an imported
//            interface admitted a (§19.3 type-confusion) case its
//            single-module twin REJECTS).
//       The byte-identical SINGLE-MODULE program (T2) REJECTS with
//       "binding pattern type's generic argument is not runtime-
//       distinguishable" — exactly the message #3161 makes the imported
//       side emit.
//
//   T3  imported `interface IHasGet<T> method Get(): T end IHasGet`
//       (a result-position member pins T — clash-protection admits), the
//       same shape
//         -> ACCEPTED pre-fix AND post-fix: the admit takes its expected
//            branch. The fix does NOT flip this case; the §19.3
//            protection admit is preserved on the imported side via the
//            symbol-shape walker instead of the AST walker.
//
// FAIL-WITHOUT / PASS-WITH was demonstrated mechanically: `git revert` of
// the ExprTypecheck.cpp diff (and the ifaceClashProtectedAllParams
// signature change), rebuild, T1 fails clean (the defect acceptance),
// T2 still rejects, T3 still accepts; re-apply, T1 rejects, T2 still
// rejects, T3 still accepts.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"

namespace bezel::sema {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md). The /tmp tmpfs races across shards; the bezel-scratch
// parent fixes that class of flake.
const fs::path& wtDir3161() {
    return wtScratchDir("wt-3161");
}

struct TempFile3161 {
    std::string path;
    explicit TempFile3161(std::string p) : path(std::move(p)) {}
    ~TempFile3161() { std::remove(path.c_str()); }
};

void writeSource3161(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// One real cross-module build: compile `libSrc` to object, then compile
// `appSrc` importing that object. Exit-code contract (mirrors #3143's
// helper so the build/test infra treats the two alike):
//   -2  the dependency artifact could not be produced (toolchain unavailable)
//       — the test skips, exactly as the other xmod tests do.
//   -1  the IMPORTING side failed to compile/link, or did not exit normally.
//   0+  the program's own exit status (WEXITSTATUS masks to 8 bits).
//
// `errOut`, when non-null, receives the IMPORTING compilation's stderr.
// Every must-REJECT case asserts the diagnostic TEXT through it, not
// merely `code == -1`: -1 is also what a link failure or an abnormal exit
// returns, so a rejection that moved to an unrelated rule would otherwise
// pass silently — the exact way a control stops controlling anything.
std::pair<int, std::string> compileImportRun3161(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag,
                                                 std::string* errOut = nullptr) {
    auto dir = wtDir3161();
    std::string stem = "bz3161_" + tag + "_" + unique_stem("");
    TempFile3161 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3161 aObj((dir / (stem + "_a.o")).string());
    writeSource3161(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3161 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3161 bExe((dir / (stem + "_b")).string());
    writeSource3161(bSrc.path, appSrc);
    TempFile3161 bErr((dir / (stem + "_b.err")).string());
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        bool ok = false;
        if (errOut) {
            std::fflush(stderr);
            int saved = ::dup(STDERR_FILENO);
            int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
            if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
            std::fflush(stderr);
            if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
            std::ifstream in(bErr.path);
            std::ostringstream ss;
            ss << in.rdbuf();
            *errOut = ss.str();
        } else {
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
        }
        if (!ok) return {-1, ""};
    }
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The single-module twin, same driver settings — for the local-vs-imported
// regression controls. Same `errOut` contract as above.
std::pair<int, std::string> compileRunLocal3161(const std::string& src,
                                                const std::string& tag,
                                                std::string* errOut = nullptr) {
    auto dir = wtDir3161();
    std::string stem = "bz3161l_" + tag + "_" + unique_stem("");
    TempFile3161 aSrc((dir / (stem + ".bzl")).string());
    TempFile3161 aExe((dir / stem).string());
    TempFile3161 aErr((dir / (stem + ".err")).string());
    writeSource3161(aSrc.path, src);
    bezel::Driver driver;
    bool okLocal = false;
    if (errOut) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                          0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *errOut = ss.str();
    } else {
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
    }
    if (!okLocal) return {-1, ""};
    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

bool has3161(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The cross-module fix-target dependency. The invariant (§12.9) interface
// shape parallels the §19.3 (B) ERASED branch in
// `typeTestInterfaceBindAmbiguous`: a class `C<T>: IFoo<T>` (parametric
// conformance ONLY, no second ground implementation) with the scrutinee
// `IFoo<erased>` and the arm `var x: C<erased>` — the §12.9 invariant match
// pins `T → erased`, equal to the pattern arg, so DISTINCT PIN does NOT fire
// and uniqueConformer = true; the function then defers to the §12.2
// clash-protection admit. Pre-#3161 the `!itd -> None` shortcut ADMITTED this
// through the imported interface without ever asking the clash-protection
// question; post-#3161 the polymorphic walker asks the question and REJECTS
// when the interface does not pin T (the INoPin case), while still ADMITTING
// when the interface DOES pin T (the IHasGet case, `Get(): T` with no params,
// so `paramsClean = true`).
//
// Note: the syntax is `: IFoo<T> end ClassName;` — the formal grammar has no
// `implements` keyword; an interface conformance IS the class's declaration
// tail (a colon-separated, base-type list, each position either a class or an
// interface). A non-final class implementing an interface that declares an
// abstract member MUST supply a body that overrides the abstract member (T3's
// `Get()` — the test would fail to compile the lib-side otherwise and the
// `return {-2, ""}` skip-mask would also skip the assertion it was supposed to
// guard).
const char* kLib3161 =
    "module NoPinLib3161; "
    "public interface INoPin3161<T> end INoPin3161; "
    "public class C3161<T> : INoPin3161<T> end C3161; "
    "public interface IHasGet3161<T> method Get(): T; end IHasGet3161; "
    "public class D3161<T> : IHasGet3161<T> "
    "  public method Get(): T => default(T); "
    "end D3161;";

// T1 — the IMPORTED, no-pin case.
//
// Mirrors T9's measure shape (variant + still-erased argument) — but with
// an INVARIANT (`IFoo<T>`) interface where the (B) ERASED branch's DISTINCT
// PIN does not fire. `C3161<T>: INoPin3161<T>` matches the §12.9 invariant
// pinning of `T → (int)->int`, equal to the pattern arg, so the loop
// completes with uniqueConformer = true; the §19.3 admit then defers to
// `ifaceClashProtectedAllParams(INoPin3161)` — FALSE on the un-pinned
// interface, REJECT. Pre-#3161 the `!itd -> None` shortcut ADMITTED this
// without asking, so cross-module contrast: fail-without/pass-with the
// fix.
//
// `typeTestInterfaceBindAmbiguous`'s entry conditions require the
// SCRUTINEE (`s`'s type) to be a generic INTERFACE, and the BINDING PATTERN
// (`var x: …`'s `declType`) to be a generic CLASS — i.e. `case ifaceVal of
// var x: cClass<args>`. The brief's `c is IFoo<int>` framing uses a
// different code path (the plain `is` pattern that #2012/#2006 guard); this
// test targets the case pattern that THIS function gates.
const char* kT1App3161 =
    "module App3161; "
    "import NoPinLib3161; "
    "public class Program "
    "  public method M(s: INoPin3161<(int) -> int>): int "
    "    case s of "
    "      var x: C3161<(int) -> int> => do return 1; end, "
    "      _ => do return 0; end "
    "    end; "
    "  end M; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// T3 — the IMPORTED, pin case. ifaceClashProtectedAllParams is TRUE on the
// symbol-shape path (a method `Get(): T` with no params, so `paramsClean =
// true` and T is pinned), so the §19.3 admit takes its expected branch and
// the erased-arg arm IS admitted. Pre-#3161 this was admitted via the `!itd
// -> None` escape hatch; post-#3161 it is admitted via the real predicate.
// Either way: the program MUST compile and run.
const char* kT3App3161 =
    "module App3161; "
    "import NoPinLib3161; "
    "public class Program "
    "  public method M(s: IHasGet3161<(int) -> int>): int "
    "    case s of "
    "      var x: D3161<(int) -> int> => do return 1; end, "
    "      _ => do return 0; end "
    "    end; "
    "  end M; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// T2 — the SINGLE-MODULE twin of T1. Must keep rejecting post-#3161: the
// fix is structural (§19.3 admit shape), not a side-effect of changing the
// admit branch. The byte-identical local program rejected before #3161 and
// must keep rejecting — pinning that the local-side path is unchanged.
const char* kT2Local3161 =
    "module M3161; "
    "interface I3161<T> end I3161; "
    "class C3161<T> : I3161<T> end C3161; "
    "public class Program "
    "  public method M(s: I3161<(int) -> int>): int "
    "    case s of "
    "      var x: C3161<(int) -> int> => do return 1; end, "
    "      _ => do return 0; end "
    "    end; "
    "  end M; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// ===========================================================================
// T1 — IMPORTED `INoPin<T>` (no member pins T), non-sealed C, erased-arg
// arm. Pre-#3161 the `!itd -> None` escape hatch admitted this
// unconditionally; post-#3161 the imported side reaches the SAME verdict as
// the local twin (T2) — REJECT with the §19.3 not-runtime-distinguishable
// diagnostic. The text-match guards against "the rejection moved to an
// unrelated rule" (e.g. a link error, which would also be -1, would pass
// the bare `code == -1` check).
//
// The scrutinee is `INoPin3161<(int)->int>` (interface, erased arg) and the
// arm is `var x: C3161<(int)->int>` (class pattern, erased arg) — exactly
// the §19.3 (B) ERASED shape `interface scrutinee, class pattern, erased
// arg`, where `ifaceClashProtectedAllParams` is the final admit gate.
// Pre-#3161 the imported side returned `IfaceBindReject::None` (admit) via
// the `!itd -> None` shortcut — a silent wrong admit that post-#3161 the
// polymorphic walker renders correctly as `ErasedAmbiguous`.
// ===========================================================================
BEZEL_TEST(xmod_iface_bind_ambiguous_3161,
           imported_unpinned_iface_rejects_erased_arg_arm) {
    std::string err;
    auto r = compileImportRun3161(kLib3161, kT1App3161, "t1", &err);
    if (r.first == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r.first, -1);
    BEZEL_EXPECT_TRUE(
        has3161(err, "binding pattern type's generic argument is not "
                     "runtime-distinguishable"));
}

// ===========================================================================
// T2 — SINGLE-MODULE twin of T1. Must keep rejecting post-#3161: pinning
// the local-side path is byte-identical. The negative regression is
// `ifaceClashProtectedAllParams(td, ctx) -> ifaceClashProtectedAllParams
// (ts, ctx)` makes any local declaration that pins T continue to pin T.
// ===========================================================================
BEZEL_TEST(xmod_iface_bind_ambiguous_3161,
           local_unpinned_iface_still_rejects_erased_arg_arm) {
    std::string err;
    auto r = compileRunLocal3161(kT2Local3161, "t2", &err);
    if (r.first == -2) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r.first, -1);
    BEZEL_EXPECT_TRUE(
        has3161(err, "binding pattern type's generic argument is not "
                     "runtime-distinguishable"));
}

// ===========================================================================
// T3 — IMPORTED `IHasGet<T> method Get(): T end` (T pinned in result
// position; `Get()` has no parameters, so the `paramsClean` check passes).
// ifaceClashProtectedAllParams is now TRUE on the symbol-shape path, so
// the §19.3 admit takes its expected branch and the erased-arg arm IS
// admitted. Pre-#3161 this was admitted via the `!itd -> None` escape
// hatch; post-#3161 it is admitted via the real predicate. Either way:
// the program MUST compile AND run. BEZEL_EXPECT_NE-style: build must NOT
// fail (`code != -1`).
// ===========================================================================
BEZEL_TEST(xmod_iface_bind_ambiguous_3161,
           imported_clash_pinned_iface_admits_erased_arg_arm) {
    std::string err;
    auto r = compileImportRun3161(kLib3161, kT3App3161, "t3", &err);
    if (r.first == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.first != -1);
}

}  // namespace bezel::sema
