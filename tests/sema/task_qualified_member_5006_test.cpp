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
//
// FOLLOW-UP (non-author review, MEASURED A/B against 7e044595): the
// alias-qualified arm's `importAliases_.find(aid->name)` check above
// matched by NAME ONLY, with no check for whether `aid` had been
// SHADOWED by a local/parameter/field of the same spelling in the
// current scope (§2.7 rule 1: a local outranks an import alias). Repro:
// `import System.Threading as T; var T := w;` (`w` a `Wrapper` whose
// OWN `Task` field is unrelated to the builtin) — `T.Task.Label` should
// resolve as an ordinary field access on the local `T`, but the gate's
// name-only alias lookup fired anyway and rejected `.Label` under the
// closed Task surface. MEASURED: compiles+runs at 8d8e6534 (pre-#5006),
// incorrectly rejected at 7e044595 (post-#5006, pre-this-fix). Fixed by
// guarding the alias lookup with `!lookupName(aid->name)`, mirroring the
// identical precedent already used by the pre-existing module-qualified-
// path-extension code a few lines above (Case (b), `partialModulePaths_`
// construction) — so an `aid` that resolves to a real scope symbol is
// never treated as the alias. Parts 7-8 below pin this: Part 7 is the
// bug's own repro (alias shadowed by a local), asserting the runtime
// VALUE read through the shadowing local, not just `r.ran`. Part 8
// checks the SIBLING module-qualified arm for the analogous hole (a
// local named `System`) — investigation showed it does NOT share the
// bug, because that arm depends entirely on `partialModulePaths_`, which
// the pre-existing Case (b) code above already gates with the same
// `lookupName` guard, so a locally-shadowed `System` never gets recorded
// as a "System.Threading" partial path in the first place. Part 8 is
// therefore a protective/confirming regression test (no code change was
// needed for it), pinning that already-correct behavior against future
// regressions in that arm.

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

// ── Part 7: alias SHADOWED by a local — the fix-forward regression ──
//
// `import System.Threading as T;` followed by `var T := w;` (a local of an
// unrelated `Wrapper` type whose OWN `Task` field is an `Inner` with a
// `Label` property, nothing to do with the builtin `Task`). Per §2.7 rule
// 1 a local outranks an import alias, so `T.Task.Label` must resolve as an
// ordinary field/property read on the LOCAL `T`, not trip the closed-Task-
// surface gate. MEASURED (non-author review, A/B against 7e044595): this
// program compiles and returns 99 at 8d8e6534 (pre-#5006) but was
// incorrectly rejected at 7e044595 (post-#5006, pre-this-fix) with the
// closed-surface `Task` diagnostic. Asserts the runtime VALUE (99), not
// merely `r.ran`, so the test cannot pass by accidentally discarding the
// read to a zero-initialized value (the same #2434 degrade class #4977/
// #5006 exist to catch).
BEZEL_TEST(task_qualified_member_5006,
           alias_shadowed_by_local_resolves_to_local_field) {
    const std::string program =
        "module M;\n"
        "import System.Threading as T;\n"
        "class Inner\n"
        "  public readonly property Label: int := 99;\n"
        "end Inner;\n"
        "class Wrapper\n"
        "  public owned readonly Task: Inner := new Inner();\n"
        "end Wrapper;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var w := new Wrapper();\n"
        "    var T := w;\n"
        "    return T.Task.Label;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(99, r.exitCode);
}

// ── Part 8: module-qualified sibling arm, local named `System` — protective
//    confirming test (investigation showed this arm does NOT share Part 7's
//    hole) ──
//
// Same shadowing shape as Part 7, but for the MODULE-qualified arm: a local
// named `System` (no alias involved) whose own `Threading.Task.Label` path
// is an ordinary field chain on the local, unrelated to the builtin. Unlike
// Part 7, this arm was NOT regressed: it depends entirely on
// `partialModulePaths_`, populated only by the pre-existing Case (b) code
// (a few lines above in this file), which already guards multi-segment
// path-building with `!lookupName(name)` — so a locally-shadowed `System`
// never gets recorded as a "System.Threading" partial path, and this
// gate's module-qualified branch correctly never fires. MEASURED: compiles
// and returns 99 both before and after the Part 7 fix. Pinned here as a
// regression guard against a future change to that shared machinery.
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_local_named_system_resolves_to_local_field) {
    const std::string program =
        "module M;\n"
        "import System.Threading;\n"
        "class Inner\n"
        "  public readonly property Label: int := 99;\n"
        "end Inner;\n"
        "class Level2\n"
        "  public owned readonly Task: Inner := new Inner();\n"
        "end Level2;\n"
        "class Level1\n"
        "  public owned readonly Threading: Level2 := new Level2();\n"
        "end Level1;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var level1 := new Level1();\n"
        "    var System := level1;\n"
        "    return System.Threading.Task.Label;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(99, r.exitCode);
}

// ── Parts 9-13: fix-forward #6079 (spec §2.3, §2.2 Aliases, §2.7 rules 1-2,
//    v0.338) ──
//
// #6079 homed `Task` in `System.Threading` (spec §2.3: "the always-available
// one by its module's qualified name (`System.Exception`;
// `System.Threading.Task` under `import System.Threading;`; §2.7 rule 2)").
// With that row the #4698 normalization rewrites an alias-qualified head
// `T.Task` to the bare built-in BEFORE the #5006 gate runs, so the gate saw a
// bare `Task` and dropped the note naming the qualifier the user wrote (Part
// 2 went red). The note's wording is still accurate under v0.338: `T` is
// resolved as an import alias by §2.7 rule 1's last source, and §2.2 makes the
// alias "an additional *qualifier*" for the module's names. These rows pin
// the EXACT note text per spelling, and its absence where no qualifier was
// written.
namespace {

// The single Error naming the missing `Task` member, or "" if there is none.
std::string unknownTaskMemberError6079(const std::string& program,
                                       const std::string& missingMember) {
    auto r = runAot(program);
    if (r.ran) return "";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member `" + missingMember + "` on `Task`") ==
                std::string::npos)
            continue;
        // The AOT harness carries each diagnostic as its rendered stderr
        // line; drop only trailing whitespace so the callers can pin how the
        // message ENDS.
        std::string msg = d.message;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' ||
                                msg.back() == ' '))
            msg.pop_back();
        return msg;
    }
    return "";
}

bool endsWith6079(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const char* const kLastSurfaceClause6079 =
    "use `Task.FromResult(...)` for an already-completed task";

}  // namespace

// ── Part 9: the Part 2 program, exact trailing note (alias spelling) ──
BEZEL_TEST(task_qualified_member_5006,
           alias_qualified_completed_task_exact_qualifier_note_6079) {
    const std::string msg = unknownTaskMemberError6079(
        "module M;\n"
        "import System.Threading as T;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := T.Task.CompletedTask;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "CompletedTask");
    BEZEL_EXPECT_TRUE(endsWith6079(
        msg,
        std::string(kLastSurfaceClause6079) +
            "; note: `Task` here is the always-available builtin type, "
            "referenced through `T.Task`, where `T` is an import alias for "
            "module `System.Threading` (\xC2\xA7" "2.7 rule 1)"));
}

// ── Part 10: another alias name and another missing member — the note names
//    the alias AS WRITTEN (`Th`), not a fixed spelling ──
BEZEL_TEST(task_qualified_member_5006,
           alias_qualified_other_unknown_member_names_written_alias_6079) {
    const std::string msg = unknownTaskMemberError6079(
        "module M;\n"
        "import System.Threading as Th;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := Th.Task.Nope;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "Nope");
    BEZEL_EXPECT_TRUE(endsWith6079(
        msg,
        std::string(kLastSurfaceClause6079) +
            "; note: `Task` here is the always-available builtin type, "
            "referenced through `Th.Task`, where `Th` is an import alias for "
            "module `System.Threading` (\xC2\xA7" "2.7 rule 1)"));
}

// ── Part 11: fully qualified head under `import System.Threading;` — exact
//    trailing note (module spelling, §2.7 rule 2), and no alias note ──
BEZEL_TEST(task_qualified_member_5006,
           module_qualified_completed_task_exact_qualifier_note_6079) {
    const std::string msg = unknownTaskMemberError6079(
        "module M;\n"
        "import System.Threading;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := System.Threading.Task.CompletedTask;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "CompletedTask");
    BEZEL_EXPECT_TRUE(endsWith6079(
        msg,
        std::string(kLastSurfaceClause6079) +
            "; note: `Task` here is the always-available builtin type, "
            "referenced through the module-qualified head "
            "`System.Threading.Task` (\xC2\xA7" "2.7 rule 2)"));
    BEZEL_EXPECT_TRUE(msg.find("import alias") == std::string::npos);
}

// ── Part 12: bare head — rejected, and NO qualifier note: the message ends
//    at the documented-surface text, with or without an (unused) alias for
//    the module in the file ──
BEZEL_TEST(task_qualified_member_5006,
           bare_completed_task_has_no_qualifier_note_6079) {
    for (const char* importLine :
         {"", "import System.Threading;\n", "import System.Threading as T;\n"}) {
        const std::string msg = unknownTaskMemberError6079(
            std::string("module M;\n") + importLine +
                "class C\n"
                "  public static method Main(): int\n"
                "    var t := Task.CompletedTask;\n"
                "    _ := t;\n"
                "    return 0;\n"
                "  end Main;\n"
                "end C;\n",
            "CompletedTask");
        BEZEL_EXPECT_TRUE(endsWith6079(msg, kLastSurfaceClause6079));
        BEZEL_EXPECT_TRUE(msg.find("note:") == std::string::npos);
    }
}

// ── Part 13: a USER class named `Task` in a user module, reached through an
//    alias — not the built-in, so the closed-surface gate (and its note) must
//    not capture it: the member the user class declares is read (42) ──
BEZEL_TEST(task_qualified_member_5006,
           alias_qualified_user_task_is_unaffected_6079) {
    const std::string libSrc =
        "module Outer.Inner;\n"
        "public static class Task\n"
        "  public static readonly property Answer: int := 42;\n"
        "end Task;\n";
    const std::string mainSrc =
        "module M;\n"
        "import Outer.Inner as U;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return U.Task.Answer;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib5006(libSrc, mainSrc));
}
