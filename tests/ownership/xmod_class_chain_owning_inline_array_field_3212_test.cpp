// fix #3212 (§15.1.1 "Inline arrays" / §28.3 "Borrow-lifetime rule" /
// §28.9 "Trivial dispose"; §2.5 "Dependency resolution"; v0.254) — the
// §28.9 trivial-dispose exemption's "no owning-inline-array field" predicate
// (MethodOwnership::classChainHasOwningInlineArrayField, src/ownership/
// Ownership.cpp:5177) walked field types off AST nodes (`fs->posParam` or
// `fs->decl`-cast `FieldDecl`) only. An IMPORTED class's instance field has
// neither (ModuleLoader never reconstructs a `FieldDecl` for a non-generic
// import — the same #503/#2414 AST-attach convention Property/Indexer
// carry), so the predicate returned false for every imported `[N owned]T?`
// field. The same predicate being false is what suppresses the §28.3
// auto-drop WARNING that local `var x := new C();` correctly fires for an
// `arr: [N owned]Cell` field.
//
// Family M4 of umbrella #3152 ("member TYPE resolution at a use site"),
// audit Appendix A.3 row `Ownership.cpp:5149` ("UNIFY with
// importedMemberType"). Of the eight sites #3212 listed as the residual
// of the #3177 sweep, this is the ONE row with a reachable defect that
// the audit's UNIFY recipe fixes cleanly; the other three reachable sites
// are explored in turn below, and the four refuted / unreachable sites
// are documented in the issue body.
//
// CAUSE. The class-chain walk in classChainHasOwningInlineArrayField only
// produces a `tr` (TypeRef) when there is an AST-side `Type` node to
// resolve. Imported members have `fs->decl == nullptr` (and a body field
// has `fs->posParam == nullptr` too). The resolver falls through to the
// `if (!ty) continue;` short-circuit and the predicate returns false.
//
// WHAT THE FIX IS. Read the wire-carried field type via
// `ctx_.importedMemberType(fs)` when `fs->decl` is null and the symbol is
// imported. The wire side encodes the type with `TypeKind::InlineArray`
// plus `arrayOwned` preserved (`Loader::convert`, ModuleLoader.cpp:432 —
// `ref->arrayOwned = mt.arrayOwned;`), published into
// the `fieldTypes_` side table that `SemaContext::importedMemberType`
// reads. The discrimination (`tr->kind == InlineArray && tr->arrayOwned`)
// is identical between local and imported — only the *source* of `tr` is
// different.
//
// WHAT GETS FIXED. The imported `var o := new C();` form with `arr:
// [N owned]Cell` now EMITS the §28.3 auto-drop WARNING the local twin
// emits, instead of silently suppressing it. Predicates that gate
// diagnostics (warnings) and not correctness are safe to widen in this
// direction — the worst-case miss would be a missed warning, which is
// exactly what the pre-fix state was.
//
// --------------------------------------------------------------------------
// MEASUREMENT ON THE PRE-FIX HEAD.
// --------------------------------------------------------------------------
//     Local twin:    var x := new LibCellClass();  → emits the warning
//                     "owned value in `x` is implicitly disposed ..."
//     Imported form: var x := new LibCellClass();  → SILENT (defect).
//
// POST-FIX:
//     Both forms emit the warning. The differential twin/import pair
//     proves the diagnostic now reaches the cross-module shape the
//     local twin has always diagnosed.
//
// The single-module twin T1b locks in the differential control: it
// compiled and warned pre-fix and must still compile and warn post-fix.
// The T0 anti-vacuity guard pins the dependency library itself builds,
// independent of the fix.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::ownership {
namespace {

namespace fs = std::filesystem;

struct TempFile3212 {
    std::string path;
    explicit TempFile3212(std::string p) : path(std::move(p)) {}
    ~TempFile3212() { std::remove(path.c_str()); }
};

void writeSource3212(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test. LibCell is a plain (trivially-disposable)
// counter with no destructor of its own, so `common::isNonTrivialDisposeClass`
// returns false for both LibCell and ClassLib3212 (the §28.9 trivial-dispose
// exemption's coarse gate). The gate that REJECTS the trivial-dispose
// exemption for ClassLib3212 is the narrower `classChainHasOwningInlineArrayField`
// predicate — ClassLib3212 declares an `[N owned]LibCell` field, so the
// predicate MUST return true, and the §28.3 auto-drop WARNING must fire
// when an unreferenced `var o := new ClassLib3212();` is the assignment
// in scope.
//
// The single-module twin assembles these declarations and the body
// verbatim from this string, so the only difference between an imported
// row and its control is the module boundary itself.

const char* kDecls3212 =
    "public class LibCell "
    "  public n: int; "
    "  public constructor() n := 1; end constructor; "
    "end LibCell; "
    "public class ClassLib3212 "
    "  public arr: [2 owned]LibCell; "
    "  public constructor() end constructor; "
    "end ClassLib3212;";

std::string libSource3212() {
    return std::string("module ImportedOwningInlineArrLib3212; ") + kDecls3212;
}

std::string appSource3212(const std::string& body) {
    return "module ImportedOwningInlineArrApp3212; "
           "import ImportedOwningInlineArrLib3212; "
           "public class ClassApp3212 " + body + " end ClassApp3212;";
}

std::string sameModuleProgram3212(const std::string& body) {
    return "module SameOwningInlineArrCtl3212; " +
           std::string(kDecls3212) +
           " public class ClassApp3212 " + body + " end ClassApp3212;";
}

// Compile the dependency, compile the app against it, run the app in a
// SEPARATE process (per tests/README.md / runAot semantics).
// `capturedStderr` receives the app compile's stderr when non-null — the
// only way to assert on diagnostic TEXT. Returns:
//    -1 compile failure
//    -2 dependency build failure
int runImported3212(const std::string& body,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3212lib_" + unique_stem("");

    TempFile3212 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3212 aObj((dir / (stem + "_a.o")).string());
    writeSource3212(aSrc.path, libSource3212());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3212 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3212 bExe((dir / (stem + "_b")).string());
    TempFile3212 bErr((dir / (stem + "_b.err")).string());
    writeSource3212(bSrc.path, appSource3212(body));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    (void)status;  // we only care that it compiled and exited; the body
                    // returns 0 either way.
    return 0;
}

// Compile and run a single-module program. Same return convention as
// runImported3212 minus the -2 case.
int runSingleModule3212(const std::string& src,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3212ctl_" + unique_stem("");
    TempFile3212 sSrc((dir / (stem + ".bzl")).string());
    TempFile3212 sExe((dir / stem).string());
    TempFile3212 sErr((dir / (stem + ".err")).string());
    writeSource3212(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    (void)status;
    return 0;
}

bool stderrMentions3212(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T0 — ANTI-VACUITY GUARD. Every row below skips when the dependency cannot
// be produced (-2), which would make the whole file pass without testing
// anything. This row asserts the dependency DOES build, so a broken
// library — e.g. if `arr: [N owned]LibCell` field syntax were removed —
// fails LOUDLY here instead of silently voiding the file.
// ===========================================================================
BEZEL_TEST(xmod_class_chain_owning_inline_array_field_3212,
           t0_dependency_library_builds) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3212dep_");
    TempFile3212 aSrc((dir / (stem + ".bzl")).string());
    TempFile3212 aObj((dir / (stem + ".o")).string());
    writeSource3212(aSrc.path, libSource3212());
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileObject(aSrc.path, aObj.path).success);
    BEZEL_EXPECT_TRUE(std::ifstream(aObj.path).good());
}

// ===========================================================================
// T1 — PRIMARY REGRESSION (issue #3212 row 6 / Ownership.cpp:5138).
// A plain `var o := new ClassLib3212();` against the IMPORTED class
// declares an owned value in a non-`using` location. Because the class
// declares an `[N owned]LibCell` field, the §28.9 trivial-dispose
// exemption's `classChainHasOwningInlineArrayField` predicate must
// disqualify it; the §28.3 auto-drop WARNING must fire.
//
// PRE-FIX: imported `var o := new ClassLib3212();` was silent
// (the predicate returned false because `fs->decl` was null).
// POST-FIX: it warns, matching the single-module twin T1b.
// ===========================================================================
BEZEL_TEST(xmod_class_chain_owning_inline_array_field_3212,
           t1_imported_owning_inline_array_field_triggers_28_3) {
    const std::string body =
        "  public static method Main(): int "
        "    var o := new ClassLib3212(); "
        "    return 0; "
        "  end Main;";
    std::string stderrText;
    int rc = runImported3212(body, &stderrText);
    if (rc == -2) return;  // dependency unavailable — T0 reports this loudly
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(stderrMentions3212(
        stderrText,
        "is implicitly disposed"));
    BEZEL_EXPECT_TRUE(stderrMentions3212(
        stderrText,
        "auto-drop"));
}

// ===========================================================================
// T1b — CONTROL. The single-module twin must continue to warn across the
// fix. If this regressed, the fix would have suppressed the local
// twin's warning (the dangerous direction).
// ===========================================================================
BEZEL_TEST(xmod_class_chain_owning_inline_array_field_3212,
           t1b_same_module_owning_inline_array_field_warns_28_3) {
    const std::string body =
        "  public static method Main(): int "
        "    var o := new ClassLib3212(); "
        "    return 0; "
        "  end Main;";
    std::string stderrText;
    BEZEL_EXPECT_EQ(
        runSingleModule3212(sameModuleProgram3212(body), &stderrText), 0);
    BEZEL_EXPECT_TRUE(stderrMentions3212(
        stderrText,
        "is implicitly disposed"));
    BEZEL_EXPECT_TRUE(stderrMentions3212(
        stderrText,
        "auto-drop"));
}

// ===========================================================================
// T2 — DISCRIMINATOR. The `using var` form (§28.9 exemption's own
// first branch — `if (ld.isUsing) return false;`) MUST continue to be
// silent for the imported form. This proves the fix widens along the
// OWNING-INLINE-ARRAY axis only — it does not silently lower the
// diagnostic gate for the `using var` shape.
// ===========================================================================
BEZEL_TEST(xmod_class_chain_owning_inline_array_field_3212,
           t2_imported_using_var_owning_inline_array_silent) {
    const std::string body =
        "  public static method Main(): int "
        "    using var o := new ClassLib3212(); "
        "    return 0; "
        "  end Main;";
    std::string stderrText;
    int rc = runImported3212(body, &stderrText);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_FALSE(stderrMentions3212(
        stderrText,
        "auto-drop"));
    BEZEL_EXPECT_FALSE(stderrMentions3212(
        stderrText,
        "is implicitly disposed"));
}

}  // namespace bezel::ownership
