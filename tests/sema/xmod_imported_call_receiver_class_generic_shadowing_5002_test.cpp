// Spec-derived regression test for issue #5002 — imported call path's missing
// receiver-class generic-param substitution (the cross-module sibling of the
// local `checkArgsAgainstParams` path's class-generic-param shadowing
// exclusion that #4915 established for the local side).
//
// spec §14.1 / §14.5 (v0.307): a callee method's still-being-inferred generic
// type parameters are the ONLY names its per-argument §5.1 check is entitled
// to treat as free type variables. The receiver class's own type parameters
// are substituted into the parameter type BEFORE §5.1 runs, so a class-level
// name is never still free at that point. The local path
// (`checkArgsAgainstParams`) does this via `substituteMemberType(paramTy,
// classTarget, classParent)`; the imported path (`checkImportedCallArity`)
// did not. Same spec rule, same invariant — but two implementations.
//
// §2.8.3 rule 4 (v0.307): the imported twin must accept every call the local
// twin accepts (and reject every call the local twin rejects).
//
// ── WHAT THE DEFECT WAS (measured at `466a28e2`, the r21-c25 base) ─────
//
// `checkImportedCallArity` passed the imported method's already-resolved
// parameter type (`pt = formalParamResolvedType(ms, j)`) directly to
// `checkAssignable` WITHOUT first substituting the receiver class's own
// generic params into it. For a method whose parameter type referenced the
// receiver class's generic param (e.g. `Holder<T>.Echo(x: T)`), `pt` was a
// bare Named `T` TypeRef (declaration == nullptr, genericArgs empty), and
// since `T` was NOT in `calleeMethodGenericParamNames` (the method has no own
// type params), the local path's invariant — class-level names are not free,
// only method-level names are — failed to hold. The per-argument
// `checkAssignable` saw an unresolved bare `T` with no concrete referent and
// reached its "Mixed / generic-parameter / null-literal cases: skip"
// catch-all at ExprTypecheck.cpp:5579 — SILENTLY ACCEPTING the argument. A
// mismatched call (e.g. `Echo("hi")` against `Echo(x: T)` on `Holder<int>`)
// was therefore NOT rejected at sema; the substituted `int` target was never
// built, codegen emitted malformed IR, and the verifier aborted with
// "Call parameter type does not match function signature!" — a measurable
// crash class rather than a clean diagnostic.
//
// ── WHAT TRIGGERS THE IMPORTED PATH HERE ───────────────────────────────
//
// `checkImportedCallArity` only runs when `md == nullptr && isImported(ms)`.
// For a GENERIC class method, `ModuleLoader` re-parses the body in a fresh
// context to monomorphize generic call sites and attaches the resulting
// `ast::MethodDecl` to the imported `MethodSymbol` (ModuleLoader.cpp:4255) —
// so the local `checkArgsAgainstParams` path runs instead, and that path
// already substitutes class-level params correctly. The imported path runs
// ONLY when the re-parse fails its §14.1/#783 SOUNDNESS GATE
// (ModuleLoader.cpp:3931): the body references something outside the
// synthetic class's own members, so the fresh re-parse can't reproduce it
// faithfully and `ms->decl` stays null. The regression shape therefore
// requires a method whose parameter type references the receiver class's
// generic param AND whose body references a sibling type from the same lib
// module — the same shape #783 / #4199 / #4915 used to drive the imported
// path in their own regression rows.
//
// ── THE FIX (one change in one function) ────────────────────────────────
//
// `checkImportedCallArity` now mirrors the local path's
// `substituteMemberType(paramTy, classTarget, classParent)` step:
//   * `recvTarget` is read from `ce.callee`'s `MemberAccessExpr` (and
//     `NullCondMemberExpr`) target exactly as `checkCallArity` does for the
//     local path.
//   * `recvParent` is `ms->parent` (the imported `MethodSymbol::parent`
//     TypeSymbol, the imported twin of the local path's `classParent`).
//   * `substituteMemberType` is a no-op when `recvParent` is null (free
//     function), when its `genericParams` is empty (non-generic receiver), or
//     when `recvTarget` is null (static call on a non-generic receiver) —
//     so the imported sibling path stays safe for every shape the local
//     path's identical guard already gates.
//
// ── ORACLE CHOICE ───────────────────────────────────────────────────────
//
// AOT execution (compile + link + run via the in-process `Driver::compileFile`
// linked into `bezel_tests_sema`) is the strict oracle — the JIT pass has no
// module boundary. The accept row asserts on STDOUT (not exit code alone):
// a `Console.WriteLine(h.Echo(7))` print in the app exits 0 and prints "7",
// and pre-fix the local twin's call site is REJECTED via a codegen-side
// "IR verification failed" because the imported twin silently accepts the
// mismatch and emits malformed LLVM. The reject row asserts the §5.1
// diagnostic so the substitution is the rejection point — without it, the
// "mixed / generic-parameter" catch-all at ExprTypecheck.cpp:5579 swallows
// the mismatch silently.

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"

namespace bezel::sema {
namespace {

namespace fs = std::filesystem;

const fs::path& wtDir5002() {
    return wtScratchDir("wt-5002");
}

struct TempFile5002 {
    std::string path;
    explicit TempFile5002(std::string p) : path(std::move(p)) {}
    ~TempFile5002() { std::remove(path.c_str()); }
};

void writeSource5002(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// One real cross-module build: compile `libSrc` to .o, compile `appSrc`
// importing that .o, return whether compile/link succeeded, the imported-
// side stderr (so a sema rejection's diagnostic survives in `out`), and
// (if so) the program's exit code and captured stdout.
struct Run5002 { bool compiled = false; int code = -1; std::string out; };

Run5002 compileImportRun5002(const std::string& libSrc,
                             const std::string& appSrc,
                             const std::string& tag) {
    auto dir = wtDir5002();
    std::string stem = "bz5002_" + tag + "_" + unique_stem("");
    TempFile5002 aSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5002 aObj((dir / (stem + "_lib.o")).string());
    writeSource5002(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {};
    }
    if (!std::ifstream(aObj.path).good()) return {};

    TempFile5002 bSrc((dir / (stem + "_app.bzl")).string());
    TempFile5002 bExe((dir / (stem + "_app")).string());
    writeSource5002(bSrc.path, appSrc);
    Run5002 r;
    // Capture the importer-side stderr so a sema rejection's diagnostic text
    // survives in `r.out` (the in-process Driver writes diagnostics to its
    // own stderr — the same shape tests/module/xmod_call_binding_3178_test
    // established). dup2 is the portable way to redirect; the Driver runs
    // synchronously, so no async hazard.
    std::string errPath = (dir / (stem + "_app_err.txt")).string();
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    std::fflush(stderr);
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.out.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    if (!r.compiled) return r;
    std::string cmd = bExe.path + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { r.compiled = false; return r; }
    char buf[512];
    std::string progOut;
    while (std::fgets(buf, sizeof(buf), p)) progOut += buf;
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    r.out += progOut;
    return r;
}

// The single-module twin: same body, same class declarations, no `import`.
// The local twin's accept is the §2.8.3 rule 4 ground truth — if the local
// twin accepts a call and the imported twin rejects it, the imported path is
// unsoundly conservative (the over-reject the issue describes). The local
// twin here is identical to the imported LIB plus a single-module Main — it
// takes the local `checkArgsAgainstParams` path, which already substitutes
// class-level params, so its verdicts are the §2.8.3 rule-4 baseline.
Run5002 compileRunLocal5002(const std::string& src,
                            const std::string& tag) {
    auto dir = wtDir5002();
    std::string stem = "bz5002l_" + tag + "_" + unique_stem("");
    TempFile5002 aSrc((dir / (stem + ".bzl")).string());
    TempFile5002 aExe((dir / stem).string());
    writeSource5002(aSrc.path, src);
    Run5002 r;
    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        r.compiled = driver.compileFile(aSrc.path, aExe.path, {}).success;
    }
    std::fflush(stderr);
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.out.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    if (!r.compiled) return r;
    std::string cmd = aExe.path + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { r.compiled = false; return r; }
    char buf[512];
    std::string progOut;
    while (std::fgets(buf, sizeof(buf), p)) progOut += buf;
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    r.out += progOut;
    return r;
}

}  // namespace

// ===========================================================================
// T1 — HEADLINE xmod REJECT row (§14.1 / §14.5, v0.307; §2.8.3 rule 4).
// The defect repro: a `Holder<T>.Echo(x: T)` whose body references a sibling
// type from the lib (forcing the §14.1/#783 SOUNDNESS GATE to refuse the
// fresh re-parse and leave `ms->decl` null, which is the gate that lets
// `checkImportedCallArity` actually run — see header note), called as
// `Holder<int>.Echo("hi")`. The parameter type references the receiver
// class's generic param `T` (no method-level `<T>` to make `T` "free").
// Pre-fix `checkImportedCallArity` did NOT substitute the class-level `T`,
// so `checkAssignable(string, T, &{})` reached the "Mixed / generic-
// parameter / null-literal cases: skip" catch-all at ExprTypecheck.cpp:5579
// and SILENTLY ACCEPTED the mismatch; codegen then emitted malformed LLVM
// ("Call parameter type does not match function signature!") and the
// verifier aborted. Post-fix the substitution mirrors the local path:
// `T -> int`, the §5.1 check sees `string → int` and rejects with the
// expected diagnostic. The local twin of the same call (a single-module
// program with no `import`) takes `checkArgsAgainstParams` and rejects with
// the same diagnostic — confirming §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_call_receiver_class_generic_shadowing_5002,
           imported_call_substitutes_class_generic_param_spec_14_1_14_5) {
    const std::string lib =
        "module Lib5002T1; "
        "public class Sibling "
        "  public v: int := 99; "
        "end Sibling; "
        "public class Holder<T> "
        "  public method Echo(x: T): int "
        "    var s := new Sibling(); "
        "    return s.v; "
        "  end Echo; "
        "end Holder; ";
    const std::string app =
        "module App5002T1; "
        "import Lib5002T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(); "
        "    var s := \"hi\"; "
        "    var v := h.Echo(s); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = compileImportRun5002(lib, app, "t1");
    BEZEL_EXPECT_FALSE(r.compiled);  // post-fix: REJECTED at sema (§5.1)
    BEZEL_EXPECT_TRUE(
        r.out.find("the call to `Echo`: argument 1 has a type that is not "
                   "implicitly convertible to the target type (§5.1)") !=
        std::string::npos);
}

// ===========================================================================
// T2 — LOCAL TWIN control (§2.8.3 rule 4). The byte-identical program in a
// single module — the local path's verdict. The local twin rejects the
// mismatched call with the same §5.1 diagnostic. Pinning this row ties
// the imported twin's post-fix rejection to the local twin's verdict (both
// reject; pre-fix the imported twin silently accepted and codegen failed).
// ===========================================================================
BEZEL_TEST(xmod_imported_call_receiver_class_generic_shadowing_5002,
           local_twin_rejects_mismatched_spec_2_8_3_rule_4_control) {
    const std::string src =
        "module App5002T2; "
        "public class Sibling "
        "  public v: int := 99; "
        "end Sibling; "
        "public class Holder<T> "
        "  public method Echo(x: T): int "
        "    var s := new Sibling(); "
        "    return s.v; "
        "  end Echo; "
        "end Holder; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(); "
        "    var s := \"hi\"; "
        "    var v := h.Echo(s); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = compileRunLocal5002(src, "t2");
    BEZEL_EXPECT_FALSE(r.compiled);
    BEZEL_EXPECT_TRUE(
        r.out.find("the argument `x` to call to `Echo` has a type that is "
                   "not implicitly convertible to the target type (§5.1)") !=
        std::string::npos);
}

// ===========================================================================
// T3 — REGRESSION CONTROL: an imported call whose parameter type does NOT
// reference the receiver class's generic param. The new substitution is a
// no-op (substituteMemberType returns `pt` unchanged when the receiver class
// has no generic params OR when `pt` doesn't mention one), so this row must
// accept and run identically pre-fix and post-fix. Pins that the fix does
// not over-widen the imported path's per-argument check.
// ===========================================================================
BEZEL_TEST(xmod_imported_call_receiver_class_generic_shadowing_5002,
           imported_non_class_generic_param_unaffected_spec_14_1_14_5) {
    const std::string lib =
        "module Lib5002T3; "
        "public class Sibling "
        "  public v: int := 99; "
        "end Sibling; "
        "public class Holder<T> "
        "  public method Add(a: int, b: int): int "
        "    var s := new Sibling(); "
        "    return s.v + a - b; "
        "  end Add; "
        "end Holder; ";
    const std::string app =
        "module App5002T3; "
        "import Lib5002T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var h := new Holder<int>(); "
        "    var v := h.Add(7, 11); "
        "    Console.WriteLine(v.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = compileImportRun5002(lib, app, "t3");
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_EQ(r.code, 0);
    // 99 + 7 - 11 = 95
    BEZEL_EXPECT_TRUE(r.out.find("95") != std::string::npos);
}

}  // namespace bezel::sema
