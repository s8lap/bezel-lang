// Regression test for issue #5006 (sema, stdlib §6.7, §2.2/§2.7, v0.307).
//
// #4977 (task_completed_task_4977_test.cpp) taught `ExprTC::resolveMemberAccess`
// to reject a BARE (uncalled) reference to an unknown `Task` static member —
// `Task.CompletedTask` and friends — but only when `ma.target` is a bare
// `IdentifierExpr` named `Task`. #5006 is the residual: the identical
// silent-degrade repro reproduces through a QUALIFIED head that names the
// exact same builtin `Task`:
//
//   (1) module-qualified:  `System.Threading.Task.CompletedTask`  (§2.7 rule 2)
//   (2) alias-qualified:   `import System.Threading as T; T.Task.CompletedTask`
//                          (§2.7 rule 1 applied to the alias)
//
// In both shapes `ma.target` is a `MemberAccessExpr`, not the bare
// `IdentifierExpr` the #4977 gate required, so the pre-#5006 gate never
// fired: sema accepted the program with only the #2434 INTERNAL
// "degraded to a zero-initialized value" warning, and a caller awaiting the
// result SIGSEGVs — the same failure mode #4977 fixed for the bare form.
// MEASURED pre-fix (this branch, `git checkout r21-4977 --`): both shapes
// compile with only the #2434 warning and exit 0 (the discard-to-zero, not
// a caught error).
//
// FIX: `ExprTC::resolveMemberAccess`'s #4977 gate (src/sema/ExprTypecheck_Calls.cpp,
// right after the §2.2/§2.7 qualified-name resolution block) now also
// recognizes `ma.target` as a `MemberAccessExpr` whose own target names the
// `System.Threading` pseudo-module — directly (via `partialModulePaths_`,
// the module-qualified-name Case (b)/(c) machinery already used by shape
// (1)) or through an import alias (`importAliases_`, shape (2)) — and
// reuses the SAME `kTaskStaticMembers` closed-surface list and diagnostic
// text #4977 established, with an added note identifying which qualified
// spelling matched.
//
// The fix does NOT touch `ma.target`'s own exprType/exprSymbol (unlike the
// §2.2/§2.7 resolution block itself, whose Case (a)/(c) arms leave the
// `System.Threading.Task` / `<Alias>.Task` chain node untyped because
// `ctx_.importedTypesOf("System.Threading")` is always null — the pseudo-
// module is never a real loaded artifact). The already-working qualified
// CALL form (`System.Threading.Task.Delay(1)`, `T.Task.Delay(1)` — MEASURED
// accepted and running to completion both before and after this fix) is
// therefore untouched by construction: nothing in the new gate can affect
// how a later CallExpr types its callee.
//
// OUT OF SCOPE, reported not fixed (see #5006 handback):
//   - `var t := Task.Delay;` (an uncalled reference to a KNOWN, overloaded
//     Task static method, no target type) is a distinct, broader,
//     pre-existing gap: spec §27.6 says a method-group reference with no
//     target type and >1 overload is an error naming the candidates, but
//     this diagnostic is entirely unimplemented (confirmed via the spec's
//     own `Math.Abs` example, which degrades identically). Not specific to
//     Task and not fixable by reusing the #4977/#5006 closed-surface gate
//     (`Delay` IS a documented member, so the gate's `known` check
//     correctly does not fire).
//   - `System.Threading.CancellationToken.None` (qualified/aliased):
//     `CancellationToken.None` genuinely exists (stdlib §6.1). Typing the
//     qualified head correctly in sema alone would still leave it broken
//     at runtime: `LlvmBackend_Exprs.cpp`'s lowering for
//     `CancellationToken.None` keys on a bare `IdentifierExpr` AST shape,
//     not on the resolved sema symbol, so a sema-only fix would be inert.
//     Fixing it for real needs a codegen change, out of scope for this
//     sema/module-only lane.

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

using bezel::test::runAot;

namespace {

// Mirrors task_completed_task_4977_test.cpp's helper: true iff sema
// rejected the program at COMPILE TIME with an Error naming both the
// missing member and `Task`'s documented surface — not merely `!r.ran`
// (which a runtime-signal-synthesized-as-Error would also satisfy).
bool rejectedAsUnknownTaskMember(const std::string& program,
                                  const std::string& missingMember) {
    auto r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member `" + missingMember + "`") ==
                std::string::npos)
            continue;
        if (d.message.find("`Task`") == std::string::npos) continue;
        return d.message.find("documented surface") != std::string::npos;
    }
    return false;
}

// Cross-module harness, mirroring task_completed_task_4977_test.cpp's
// `buildAndRunWithLib`: compile `libSrc` to a library object, compile
// `mainSrc` against it as a dependency (§2.5), link, run, and return the
// exit code — -1 on any compile, link or run failure.
int buildAndRunWithLib5006(const std::string& libSrc,
                            const std::string& mainSrc) {
    const std::filesystem::path& dir = wtScratchDir("wt-5006");
    const std::string stem = unique_stem("bezel_5006_");
    const std::string libBzl = (dir / (stem + "_lib.bzl")).string();
    const std::string libObj = (dir / (stem + "_lib.o")).string();
    const std::string mainBzl = (dir / (stem + "_main.bzl")).string();
    const std::string exe = (dir / stem).string();
    { std::ofstream(libBzl) << libSrc; }
    { std::ofstream(mainBzl) << mainSrc; }
    bezel::Driver driver;
    if (!driver.compileObject(libBzl, libObj).success) return -1;
    std::vector<bezel::Dependency> deps;
    deps.push_back(bezel::Dependency{libObj, libObj});
    if (!driver.compileFile(mainBzl, exe, deps).success) return -1;
    int status = std::system(exe.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ── Part 1: module-qualified head — `System.Threading.Task.CompletedTask` ──
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_completed_task_is_rejected) {
    const std::string program =
        "module M;\n"
        "import System.Threading;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := System.Threading.Task.CompletedTask;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(rejectedAsUnknownTaskMember(program, "CompletedTask"));
    // The diagnostic must identify WHICH qualified spelling matched, per
    // §2.7 rule 2 — not just fall back to reading as the bare form.
    auto r = runAot(program);
    bool sawQualifierNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("module-qualified head "
                            "`System.Threading.Task`") != std::string::npos &&
            d.message.find("\xC2\xA7" "2.7 rule 2") != std::string::npos)
            sawQualifierNote = true;
    }
    BEZEL_EXPECT_TRUE(sawQualifierNote);
}

// ── Part 2: alias-qualified head — `T.Task.CompletedTask` ──
BEZEL_TEST(task_qualified_member_5006,
           alias_qualified_completed_task_is_rejected) {
    const std::string program =
        "module M;\n"
        "import System.Threading as T;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := T.Task.CompletedTask;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_TRUE(rejectedAsUnknownTaskMember(program, "CompletedTask"));
    auto r = runAot(program);
    bool sawQualifierNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`T.Task`") != std::string::npos &&
            d.message.find("import alias for module "
                            "`System.Threading`") != std::string::npos &&
            d.message.find("\xC2\xA7" "2.7 rule 1") != std::string::npos)
            sawQualifierNote = true;
    }
    BEZEL_EXPECT_TRUE(sawQualifierNote);
}

// ── Part 3: a bogus member via a qualified head is likewise rejected
//    (generalizes beyond the one literal spelling, mirroring #4977 Part 2) ──
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_other_unknown_member_is_rejected) {
    const std::string program =
        "module M;\n"
        "import System.Threading;\n"
        "class C public static method Main(): int\n"
        "  var t := System.Threading.Task.TotallyBogusProperty;\n"
        "  _ := t;\n"
        "  return 0;\n"
        "end Main; end C;\n";
    BEZEL_EXPECT_TRUE(
        rejectedAsUnknownTaskMember(program, "TotallyBogusProperty"));
}

// ── Part 4: accept control — the module-qualified CALL form still works ──
//
// The fix must not regress the already-working qualified CALL form: the
// new gate is `&ma != calleeOfCallCtx_`-scoped exactly like #4977's, and
// derives its verdict without mutating `ma.target`'s own exprType, so
// `System.Threading.Task.Delay(1)` — MEASURED to compile and run cleanly
// both before and after this fix — must keep doing so.
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_delay_call_form_still_runs) {
    const std::string program =
        "module M;\n"
        "import System.Threading;\n"
        "class C\n"
        "  public static async method Main(): Task<int>\n"
        "    await System.Threading.Task.Delay(1);\n"
        "    return 42;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── Part 5: accept control — the alias-qualified CALL form still works ──
BEZEL_TEST(task_qualified_member_5006,
           alias_qualified_delay_call_form_still_runs) {
    const std::string program =
        "module M;\n"
        "import System.Threading as T;\n"
        "class C\n"
        "  public static async method Main(): Task<int>\n"
        "    await T.Task.Delay(1);\n"
        "    return 42;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── Part 6: a module-qualified user type named `Task` is unaffected ──
//
// BUILTIN-ONLY GATE, extended to the qualified form: the new arm only
// fires when the intermediate node's recorded partial module path (or
// alias target) is EXACTLY "System.Threading" — a user module whose
// dotted name happens to end in ".Task" at the same nesting depth must not
// trip this gate. `partialModulePaths_` is compared by full dotted string
// equality against "System.Threading" specifically (not a suffix/contains
// match), so a differently-named user module's `Task` type is untouched;
// this pins that with a 3-segment user module path so the shape (module
// name + "Task" member) matches structurally.
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_user_task_three_segment_is_unaffected) {
    // Reuses the cross-module harness pattern from #4977 Part 7, but with
    // a deeper module path so the qualified head has the same 2-level
    // MemberAccessExpr shape (`Seg1.Seg2.Task`) as `System.Threading.Task`.
    const std::string libSrc =
        "module Outer.Inner;\n"
        "public static class Task\n"
        "  public static readonly property Answer: int := 42;\n"
        "end Task;\n";
    const std::string mainSrc =
        "module M;\n"
        "import Outer.Inner;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return Outer.Inner.Task.Answer;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib5006(libSrc, mainSrc));
}
