// codegen/sema — fix #5012, #5013 (spec §2.7 "Qualified names denote the
// same entity", v0.307; stdlib §6.7 `Task`, §6.1 `CancellationToken`): a
// module-qualified (`System.Threading.Task.FromResult(...)`,
// `System.Threading.CancellationToken.None`) or alias-qualified
// (`<alias>.Task.FromResult(...)`, `<alias>.CancellationToken.None`, under
// `import System.Threading as <alias>;`) spelling of these two builtin
// static-dispatch heads missed the special case codegen (and sema's
// `typeCall` `colName` receiver-name lambda) use to recognize them, because
// that special case matched only a bare `IdentifierExpr` receiver. §2.2
// makes `Task` and `CancellationToken` public bare names of
// `System.Threading`, and §2.7 makes a qualified spelling denote the SAME
// entity as the bare one — so the qualified/aliased spellings must type and
// lower identically.
//
// SCOPE, relative to #5006 (base: r21-c27s, commits b1b2510f + 6abdb2c2):
// #5006 already fixed the REJECT half of this same gap — an unknown static
// member on a qualified/aliased `Task` head (`System.Threading.Task.
// CompletedTask`) — by re-deriving the qualifier fact directly at its
// reject-gate (`partialModulePaths_` / `importAliases_`), deliberately
// WITHOUT mutating `ma.target`'s exprType/exprSymbol (its own commit
// message says so explicitly). #5006's commit message also explicitly
// scopes OUT the two things left: qualified/aliased generic static methods
// (`Task.FromResult`, filed as #5012) and qualified/aliased
// `CancellationToken.None` (#5013), both "needs a codegen change, out of
// scope". This commit is exactly that remainder — the ACCEPT-path half.
//
// ONE combined commit, ONE shared mechanism for #5012+#5013: both are the
// same underlying gap (a bare-`IdentifierExpr`-only receiver match) reached
// through the SAME new helper, `ExprTC::isQualifiedBuiltinThreadingHead`
// (src/sema/ExprTypecheck_impl.hpp / ExprTypecheck_Calls.cpp), which DOES
// stamp the qualified/aliased head node's exprSymbol with the SAME builtin
// `TypeSymbol*` the bare identifier gets (`ExprTypecheck_Exprs.cpp`'s
// `setExprSymbol(&e, builtinClassSym(name))`) — #5006's choice not to do
// this for its own narrower reject-only fix does not preclude this commit
// doing it for the accept path, since neither reads the other's output
// (different AST nodes / different resolveMemberAccess invocations; see
// the per-hunk comments). Both sema's `typeCall` `colName` lambda and
// codegen (LlvmBackend_Calls.cpp / LlvmBackend_Exprs.cpp) read the stamp
// back by symbol identity — never by re-deriving the qualifier's AST shape
// a second time. The shadow guard (`!lookupName(aid->name)`, §2.7 rule 1)
// mirrors the identical precedent 6abdb2c2 (#5006 non-author review) added
// to the reject-gate's own alias arm.
//
// PRE-FIX BEHAVIOUR (measured, not assumed — see the FAIL-WITHOUT recipe
// below):
//   * `var v: int := await System.Threading.Task.FromResult(41);` compiled
//     successfully but printed a wrong value: `colName` returned "" for the
//     qualified receiver, `typeCall` never resolved `FromResult`'s return
//     type, the call's `lowerExpr` returned null, and `lowerLocalDecl`'s
//     defense-in-depth zero-init fallback stored 0 in `v`'s slot — silently,
//     behind an `internal: ... — #2434` Warning (never an Error).
//   * `System.Threading.CancellationToken.None` degraded the SAME way; its
//     zero-initialized fallback happens to be bit-identical to the correct
//     null-pointer `.None` value, so — uniquely for this shape — the RUNTIME
//     VALUE cannot distinguish fixed from unfixed. The mechanical FAIL-
//     WITHOUT oracle for that specific case is the #2434 diagnostic's
//     presence (pre-fix) vs. absence (post-fix) at COMPILE time, which
//     `runAot` discards on a successful compile (tests/README.md,
//     `scripts/check_internal_residual.py`'s docstring, "CANNOT SEE - gtest
//     programs") — so that half of the oracle is pinned in
//     tests/lit/qualified_builtin_threading_*_no_degrade_5012_5013.bzl
//     instead, which drives the real `bezelc` CLI and inspects its raw
//     stderr. This file carries the VALUE-observable half plus every
//     accept/reject control; the two lit files carry the diagnostic-absence
//     half for CancellationToken.None.
//
// FAIL-WITHOUT (mechanical, performed against this exact commit's parent):
//   copy the 4 fixed files (ExprTypecheck_impl.hpp, ExprTypecheck_Calls.cpp,
//   LlvmBackend_Calls.cpp, LlvmBackend_Exprs.cpp) to scratch, `git checkout
//   <this commit's parent> -- <file>` for each, rebuild the codegen test
//   binary, and re-run this filter: every qualified/aliased `*_5012`/
//   `*_5013` row below FAILS (the FromResult rows assert 42 and get 1; the
//   CancellationToken shadowing/user-module controls are unaffected because
//   they never touch the new code path). Restore the fixed files and
//   rebuild: every row PASSES.
//
// A BONUS OBSERVATION, not a separately tested claim: the SAME widening
// (sema's `colName` / codegen's `cls`, keyed off the newly-stamped exprSymbol
// rather than a bare-identifier re-derivation) also repairs
// `System.Threading.Task.Delay(...)` reached through a qualified/aliased
// head, which pre-fix silently no-op'd as a bare `ExpressionStmt` with NO
// diagnostic at all (a strictly worse silent failure than #5012's warned
// degradation — `LlvmBackend_Stmts.cpp`'s bare-statement path lowers for
// side effects only and drops a null `lowerExpr` result unguarded). Not
// pinned here: a sound deterministic oracle for "did the delay actually
// happen" needs wall-clock timing, which this project's test discipline
// treats as flaky-by-construction; the two filed issues do not ask for
// `Task.Delay` coverage, so this stays an observation rather than a new
// test surface (CLAUDE.md "No 'while we're at it' expansions").

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// ---------------------------------------------------------------------------
// Cross-module harness (accept-controls: user module / user class sharing a
// builtin name) — the same Driver::compileObject + Driver::compileFile(deps)
// shape tests/codegen/alias_owned_param_keeps_auto_drop_3035_test.cpp uses.
// ---------------------------------------------------------------------------

const std::filesystem::path& wtDir5012() {
    return wtScratchDir("wt-5012-5013");
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

// The process's exit code is WEXITSTATUS-masked to 8 bits (CLAUDE.md: "Never
// assert a value above 255 — move the observable to stdout"), so a test
// whose expected VALUE exceeds 255 must read it off stdout instead of the
// exit code. Returns the captured stdout+stderr text; the exit code is
// asserted to be 0 separately by the caller.
std::string runCaptureStdout(const std::string& exe, int* exitCodeOut) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) {
        if (exitCodeOut) *exitCodeOut = -1;
        return out;
    }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    if (exitCodeOut)
        *exitCodeOut = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return out;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib5012(const std::filesystem::path& dir,
                       const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_5012lib_");
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

InProcessApp buildApp5012(const std::filesystem::path& dir,
                          const std::string& libObjPath,
                          const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_5012app_");
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
// #5012 — module-qualified / alias-qualified `Task.FromResult`, awaited.
// Oracle: the awaited value. Pre-fix: 1 (zero-init fallback + 1). Post-fix:
// 42 (41 + 1), matching the bare-form control.
// ===========================================================================

BEZEL_TEST(qualified_builtin_threading_head_5012,
           module_qualified_task_fromresult_awaited_returns_42_stdlib_6_7) {
    const std::string src =
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var v: int := await System.Threading.Task.FromResult(41); "
        "    return v + 1; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

BEZEL_TEST(qualified_builtin_threading_head_5012,
           alias_qualified_task_fromresult_awaited_returns_42_stdlib_6_7) {
    const std::string src =
        "module M; import System.Threading as T; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var v: int := await T.Task.FromResult(41); "
        "    return v + 1; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

BEZEL_TEST(qualified_builtin_threading_head_5012,
           bare_task_fromresult_control_still_returns_42_stdlib_6_7) {
    // Bare-form control (#4977's own shape): must be unaffected by the widened
    // `colName` / exprSymbol match.
    const std::string src =
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var v: int := await Task.FromResult(41); "
        "    return v + 1; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ===========================================================================
// #5013 — module-qualified / alias-qualified `CancellationToken.None`.
// The runtime VALUE ("False") does not distinguish fixed from unfixed (see
// the file header) — these rows pin base correctness and the accept/reject
// controls; tests/lit/qualified_builtin_threading_*_no_degrade_5012_5013.bzl
// carry the diagnostic-absence half that IS the FAIL-WITHOUT oracle here.
// ===========================================================================

BEZEL_TEST(qualified_builtin_threading_head_5013,
           module_qualified_cancellation_token_none_reports_not_cancelled_stdlib_6_1) {
    const std::string src =
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    var t: CancellationToken := System.Threading.CancellationToken.None; "
        "    Console.WriteLine(t.IsCancellationRequested); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("False") != std::string::npos);
}

BEZEL_TEST(qualified_builtin_threading_head_5013,
           alias_qualified_cancellation_token_none_reports_not_cancelled_stdlib_6_1) {
    const std::string src =
        "module M; import System.Threading as T; "
        "class C "
        "  public static method Main(): int "
        "    var t: CancellationToken := T.CancellationToken.None; "
        "    Console.WriteLine(t.IsCancellationRequested); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("False") != std::string::npos);
}

BEZEL_TEST(qualified_builtin_threading_head_5013,
           bare_cancellation_token_none_control_stdlib_6_1) {
    const std::string src =
        "module M; import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    var t: CancellationToken := CancellationToken.None; "
        "    Console.WriteLine(t.IsCancellationRequested); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("False") != std::string::npos);
}

// NOTE: the reject-gate half of this shape (`System.Threading.Task.
// CompletedTask` etc.) is already fully covered by fix #5006
// (tests/sema/task_qualified_member_5006_test.cpp) and is not re-tested
// here — this commit's `isQualifiedBuiltinThreadingHead` helper contributes
// only the ACCEPT-path exprSymbol stamp that #5006 deliberately did not add
// (its own commit message: "Nothing mutates `ma.target`'s own exprType/
// exprSymbol"), so a reject-side row here would pass identically with or
// without this commit's diff and pin nothing new.

// ===========================================================================
// Shadowing accept-control: a local variable named `T` shadows the import
// alias `T` for `System.Threading` (§2.7 rule 1 — local scope precedes
// import aliases). `T.Task` must resolve against the LOCAL `int`, never the
// builtin, so the compile is rejected for "no member `Task` on `int`" — NOT
// treated as the builtin Task class.
// ===========================================================================

BEZEL_TEST(qualified_builtin_threading_head_5012,
           local_variable_shadowing_alias_is_not_treated_as_builtin_2_7) {
    auto r = runAot(
        "module M; import System.Threading as T; "
        "class C "
        "  public static method Main(): int "
        "    var T: int := 5; "
        "    var v := T.Task.FromResult(41); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawPrimitiveNoMember = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("Task") != std::string::npos &&
            d.message.find("int") != std::string::npos) {
            sawPrimitiveNoMember = true;
        }
    }
    BEZEL_EXPECT_TRUE(sawPrimitiveNoMember);
}

// Stronger shadowing control (non-author review, 2026-09-26): the previous
// row proves SOME error fires when the shadowing local's type has no member
// named `Task`, which alone does not rule out a subtly-wrong shadow guard
// that happens to fire an unrelated error. This row makes the local's type
// actually HAVE a `Task` member, so a correct `!lookupName(aid->name)` guard
// must resolve `T.Task` as an ordinary field read on the LOCAL and produce
// its real VALUE (99) — a wrong guard that still treated `T` as the
// `System.Threading` alias would instead try (and fail, or silently
// degrade) to resolve `Task` as the builtin static-helper class.
BEZEL_TEST(qualified_builtin_threading_head_5012,
           local_shadowing_alias_with_own_task_member_reads_local_value_2_7) {
    const std::string src =
        "module M; import System.Threading as T; "
        "class Box public Task: int := 99; end Box; "
        "class C "
        "  public static method Main(): int "
        "    var T := new Box(); "
        "    return T.Task; "
        "  end Main; "
        "end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(99, r.exitCode);
}

// ===========================================================================
// User-module accept-control: a module named `System.Threading.Tasks`
// (PLURAL — a real, distinct user module, not the builtin pseudo-module)
// with its OWN class `Task` and `FromResult` must dispatch to the USER's
// method, never the builtin — proves the immediate-qualifier-must-equal-
// exactly-"Threading" guard in `isQualifiedBuiltinThreadingHead` (it must
// reject "Tasks" as a match for "Threading").
//
// An earlier version of this test hit an UNRELATED, pre-existing defect
// (filed as #5025): the library class here must be `public` — a
// module-private class referenced via a cross-module QUALIFIED static call
// silently degrades to a zero/null value (an internal `#2491`/`#3079`
// warning, no diagnostic-visible error) instead of a compile-time
// accessibility error, regardless of the class's name (confirmed on this
// commit's parent, 6abdb2c2, and confirmed unrelated to the class being
// named `Task` specifically — an unrelated name/module without `public`
// reproduces identically, and `Task`/`MyLib` WITH `public` work correctly).
// That defect is orthogonal to what this test verifies (the module-path
// prefix match), so the library class below is `public`.
//
// Oracle observable: the value is printed via `Console.WriteLine` and read
// off stdout, not the process exit code — `1041` exceeds 255 and would be
// silently truncated by `WEXITSTATUS` (CLAUDE.md: "Never assert a value
// above 255 — move the observable to stdout").
// ===========================================================================

BEZEL_TEST(qualified_builtin_threading_head_5012,
           user_module_threading_tasks_plural_dispatches_to_user_type_2_7) {
    const auto& dir = wtDir5012();
    const std::string libSrc =
        "module System.Threading.Tasks; "
        "public class Task "
        "  public static method FromResult(x: int): int "
        "    return x + 1000; "
        "  end FromResult; "
        "end Task;";
    LibHandle lib = buildLib5012(dir, libSrc);
    BEZEL_EXPECT_TRUE(lib.ok);
    if (!lib.ok) return;

    const std::string appSrc =
        "module M; import System.Threading.Tasks; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(System.Threading.Tasks.Task.FromResult(41)); "
        "    return 0; "
        "  end Main; "
        "end C;";
    InProcessApp app = buildApp5012(dir, lib.objPath, appSrc);
    BEZEL_EXPECT_TRUE(app.ok);
    if (!app.ok) return;
    int exitCode = -1;
    std::string out = runCaptureStdout(app.exePath, &exitCode);
    BEZEL_EXPECT_EQ(0, exitCode);
    BEZEL_EXPECT_TRUE(out.find("1041") != std::string::npos);
}

// ===========================================================================
// User-class accept-control: a user class literally named `Task` in an
// UNRELATED module must dispatch to the user's own `FromResult`, never the
// builtin — this class carries a real `decl`/`moduleName`, so
// `ctx_.builtinClassSym("Task")` (a synthetic symbol with no `decl`) can
// never compare equal to it.
//
// An earlier version of this test (library class not `public`) hit the same
// unrelated #5025 defect as the test above and had been rewritten to avoid
// the literal name `Task` entirely as a workaround; #5025's root cause
// turned out to be the missing `public`, not the name, so the literal-name
// coverage is restored here with `public class Task`.
//
// Oracle observable: stdout, not exit code, for the same reason as above.
// ===========================================================================

BEZEL_TEST(qualified_builtin_threading_head_5012,
           user_class_named_task_in_other_module_dispatches_to_user_type_2_7) {
    const auto& dir = wtDir5012();
    const std::string libSrc =
        "module MyLib; "
        "public class Task "
        "  public static method FromResult(x: int): int "
        "    return x + 2000; "
        "  end FromResult; "
        "end Task;";
    LibHandle lib = buildLib5012(dir, libSrc);
    BEZEL_EXPECT_TRUE(lib.ok);
    if (!lib.ok) return;

    const std::string appSrc =
        "module M; import MyLib; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(MyLib.Task.FromResult(41)); "
        "    return 0; "
        "  end Main; "
        "end C;";
    InProcessApp app = buildApp5012(dir, lib.objPath, appSrc);
    BEZEL_EXPECT_TRUE(app.ok);
    if (!app.ok) return;
    int exitCode = -1;
    std::string out = runCaptureStdout(app.exePath, &exitCode);
    BEZEL_EXPECT_EQ(0, exitCode);
    BEZEL_EXPECT_TRUE(out.find("2041") != std::string::npos);
}
