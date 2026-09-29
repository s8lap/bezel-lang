// Regression test for issue #4977 (sema, v0.307).
//
// `Task.CompletedTask` does not exist. `Bezel_For_Code_Generation.md` §5
// lists it explicitly among the invented APIs a model reaches for that the
// stdlib does not define, naming `Task.FromResult` as the real replacement;
// the stdlib §6.7 `Task` class declares no such member either — `Task` has
// no static PROPERTIES at all, only static methods (Delay, FromResult,
// FromOwnedResult, FromException, FromCancelled, WhenAll/WhenAllOwned,
// WhenAny, WhenEach/WhenEachOwned, FireAndForget).
//
// Before the fix, sema accepted `Task.CompletedTask` silently: a BARE
// (uncalled) `Task.<member>` reference reaches `ExprTC::resolveMemberAccess`
// — a different code path from the CallExpr-specific `kStaticSurfaces`
// closed-surface table that #3021/#3024 added for the CALL form
// (`Task.TotallyBogusMember(1, 2)`, see
// `tests/sema/static_class_closed_surface_3024_test.cpp`). No arm in
// `resolveMemberAccess` recognized any `Task` member (valid or not), so an
// unrecognized bare reference fell through to the function's final silent
// `return;` — no exprType, no diagnostic. Per the issue's own MEASURED
// repro, the resulting `return Task.CompletedTask;` then failed to lower in
// codegen and silently degraded to a zero/null return value (the #2491
// "degraded to a zero/null return value" INTERNAL warning, not an Error) —
// the program still compiled and exited 0. A caller that `await`s the
// result of a method declared to return that degraded value then SIGSEGVs,
// because a zero/null value is not a valid `Task` the runtime can suspend
// on.
//
// FIX: `ExprTC::resolveMemberAccess` (src/sema/ExprTypecheck_Calls.cpp) now
// recognizes a bare `Task.<member>` reference (gated to the non-call form
// via `calleeOfCallCtx_`, so it never double-diagnoses or interferes with
// the CALL-form `kStaticSurfaces` table) and rejects any member outside the
// documented static-method surface with an Error naming the surface
// (stdlib §6.7). Mirrors the `CancellationToken.None` / `Encoding.<X>`
// closed-surface rejects already in the same function.
//
// BUILTIN-ONLY GATE: mirrors #3024's `isBuiltinStatic` gate — a user class
// legitimately named `Task` (pinned by
// `static_class_closed_surface_3024_test.cpp`'s
// `user_class_with_same_name_as_builtin_is_unaffected` case for the CALL
// form) must be unaffected for the BARE-reference form too; this file adds
// that pin.

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

// True iff sema rejected the program at COMPILE TIME (not a runtime
// SIGSEGV synthesized as an Error by runAot — see
// static_class_closed_surface_3024_test.cpp's helper for the same
// precedent) with an Error naming both the missing member and `Task`'s
// documented surface.
bool rejectedAsUnknownTaskMember(const std::string& program,
                                  const std::string& missingMember) {
    auto r = runAot(program);
    if (r.ran) return false;  // accepted (or crashed at runtime) — failure
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

// Cross-module harness for the imported-user-`Task` rows (Parts 6–8):
// compile `libSrc` to a library object, compile `mainSrc` against it as a
// dependency (§2.5), link, run, and return the exit code — -1 on any
// compile, link or run failure. Scratch lives under the worktree scratch
// dir, never /tmp.
struct TempFile4977 {
    std::string path;
    explicit TempFile4977(std::string p) : path(std::move(p)) {}
    ~TempFile4977() { std::remove(path.c_str()); }
};

int buildAndRunWithLib(const std::string& libSrc, const std::string& mainSrc) {
    const std::filesystem::path& dir = wtScratchDir("wt-4977");
    const std::string stem = unique_stem("bezel_4977_");
    TempFile4977 libBzl((dir / (stem + "_lib.bzl")).string());
    TempFile4977 libObj((dir / (stem + "_lib.o")).string());
    TempFile4977 mainBzl((dir / (stem + "_main.bzl")).string());
    TempFile4977 exe((dir / stem).string());
    { std::ofstream(libBzl.path) << libSrc; }
    { std::ofstream(mainBzl.path) << mainSrc; }
    bezel::Driver driver;
    if (!driver.compileObject(libBzl.path, libObj.path).success) return -1;
    std::vector<bezel::Dependency> deps;
    deps.push_back(bezel::Dependency{libObj.path, libObj.path});
    if (!driver.compileFile(mainBzl.path, exe.path, deps).success) return -1;
    int status = std::system(exe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* const kLibWithUserTask =
    "module Lib;\n"
    "public static class Task\n"
    "  public static readonly property Answer: int := 42;\n"
    "end Task;\n";

}  // namespace

// ── Part 1: the issue's exact repro is rejected at compile time ──
//
// Per #4977's MEASURED repro: a non-async method returns
// `Task.CompletedTask`; the caller awaits the result and SIGSEGVs. The
// fix must catch this at the `return Task.CompletedTask;` site, before any
// SIGSEGV is reachable.
BEZEL_TEST(task_completed_task_4977,
           issue_repro_is_rejected_with_unknown_member_diagnostic) {
    const std::string program =
        "module M;\n"
        "class H\n"
        "  public static method OnA(x: int): Task\n"
        "    Console.WriteLine($\"a:{x}\");\n"
        "    return Task.CompletedTask;\n"
        "  end OnA;\n"
        "end H;\n"
        "class C\n"
        "  public static async method Main(): Task<int>\n"
        "    await H.OnA(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    // Must be rejected at COMPILE TIME — `r.ran` false and no runtime
    // signal. A weaker oracle (just `!r.ran`) would also pass if the
    // pre-fix SIGSEGV were merely left in place and runAot's
    // signal-to-Error synthesis papered over it; assert the specific
    // sourced diagnostic instead (per CLAUDE.md's vacuous-oracle catalogue).
    BEZEL_EXPECT_TRUE(
        rejectedAsUnknownTaskMember(program, "CompletedTask"));
}

// ── Part 2: a bare reference to an unrelated bogus Task member is
//    likewise rejected (generalizes beyond the one literal spelling) ──
BEZEL_TEST(task_completed_task_4977,
           bare_reference_to_other_unknown_task_member_is_rejected) {
    const std::string program =
        "module M;\n"
        "class C public static method Main(): int\n"
        "  var t := Task.TotallyBogusProperty;\n"
        "  _ := t;\n"
        "  return 0;\n"
        "end Main; end C;\n";
    BEZEL_EXPECT_TRUE(
        rejectedAsUnknownTaskMember(program, "TotallyBogusProperty"));
}

// ── Part 3: the documented CALL-form surface is unaffected ──
//
// A false reject is worse than the bug it fixes. The fix must not touch
// the CALL form (`Task.FromResult(...)`, `Task.Delay(...)`) — that form is
// validated independently by the pre-existing #3021/#3024
// `kStaticSurfaces` table, and the new bare-reference check is gated off
// via `calleeOfCallCtx_` specifically so it never fires for a call callee.
BEZEL_TEST(task_completed_task_4977,
           documented_call_form_still_compiles) {
    const std::string program =
        "module M;\n"
        "class H\n"
        "  public static method OnA(x: int): Task<int>\n"
        "    return Task.FromResult(x);\n"
        "  end OnA;\n"
        "end H;\n"
        "class C\n"
        "  public static async method Main(): Task<int>\n"
        "    var v := await H.OnA(41);\n"
        "    return v + 1;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── Part 4: the §11.3 collision invariant, for the BARE-reference form ──
//
// static_class_closed_surface_3024_test.cpp's
// `user_class_with_same_name_as_builtin_is_unaffected` pins the CALL form
// (`Task.Hello()`) for a user class named `Task`. This fix touches a
// DIFFERENT code path (the bare-reference arm in `resolveMemberAccess`),
// so it needs its own pin: a user class named `Task` with a bare static
// readonly property must resolve to the user's own member, not trip the
// builtin's closed-surface reject.
BEZEL_TEST(task_completed_task_4977,
           user_class_named_task_bare_property_is_unaffected) {
    const std::string program =
        "module M;\n"
        "public static class Task\n"
        "  public static readonly property CompletedTask: int := 42;\n"
        "end Task;\n"
        "class C public static method Main(): int\n"
        "  return Task.CompletedTask;\n"
        "end Main; end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(42, r.exitCode);
}

// ── Part 5: an import alias named `Task` does NOT shadow the builtin ──
//
// narrow #4977. Spec §2.7 rule 1 (v0.307): a chain head resolves through
// "local scope, then the enclosing type's accessible members ..., then the
// current module's declarations, then unambiguous imported bare names (per
// §2.2's collision rule), then import **aliases**", with no backtracking.
// `Task` is a public of `System.Threading` (stdlib §6, §6.7) and is always
// available (§2.3), and §2.2 says the aliased module's "publics are still
// visible by bare name" — so in `import System.Threading as Task;` the head
// `Task` names the builtin TYPE, the alias is unreachable, and
// `Task.CancellationToken` asks the builtin `Task` for a member it lacks.
// (§2.2 makes the alias declaration itself an error — "an alias may not
// collide with ... an unambiguously imported public bare name"; that
// declaration-site check is tracked separately, not in this fix.)
//
// Before this narrowing the reject fired but read as if `Task` were the
// module ("no member `CancellationToken` on `Task` ... Task has no static
// properties"). The diagnostic must now say that `Task` names the builtin
// and that the alias does not shadow it (§2.7), naming the aliased module.
// On origin/main 466a28e2 this program compiled with only an INTERNAL
// "degraded to a zero-initialized value" warning — the same silent
// fall-through as the issue itself — so the reject is the spec verdict.
BEZEL_TEST(task_completed_task_4977,
           import_alias_named_task_does_not_shadow_builtin_spec_2_7) {
    const std::string program =
        "module M;\n"
        "import System.Threading as Task;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var t := Task.CancellationToken.None;\n"
        "    _ := t;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawAliasNote = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member `CancellationToken` on `Task`") ==
            std::string::npos)
            continue;
        if (d.message.find("always-available builtin type `Task`") !=
                std::string::npos &&
            d.message.find("import alias `Task` for module "
                           "`System.Threading`") != std::string::npos &&
            d.message.find("\xC2\xA7" "2.7 rule 1") != std::string::npos &&
            d.message.find("does not shadow the builtin") != std::string::npos)
            sawAliasNote = true;
    }
    BEZEL_EXPECT_TRUE(sawAliasNote);
}

// ── Part 6: a user type named `Task` IMPORTED from another module ──
//
// FLIPPED by #5140 (§2.3, v0.309). Pre-#5140 this row passed: `import Lib;`
// made Lib's public `Task` visible by bare name (§2.2), the IdentifierExpr
// visitor found it by scope lookup ahead of the builtin fallback, and
// `Task.Answer` ran to 42. The §2.3 fix (every-row-alike ambiguity rule)
// makes the bare name ambiguous: each side is reachable by qualification
// (the imported public via `Lib.Task`, the always-available via
// `System.Threading.Task`). Part 7 (module-qualified `Lib.Task`) and Part
// 8 (alias-qualified `L.Task`) still succeed unchanged — both are
// qualification forms. Part 5 (alias named `Task`) is also untouched —
// the alias-naming-itself path is a separate §2.2 rule.
//
// The previous comment's "spec question only for the
// System.Collections.Generic row" parenthetical is now obsolete: #5140
// applies the rule "every row alike" per spec §2.3 (v0.309).
BEZEL_TEST(task_completed_task_4977,
           imported_user_task_bare_static_property_is_ambiguous) {
    const std::string mainSrc =
        "module M;\n"
        "import Lib;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return Task.Answer;\n"
        "  end Main;\n"
        "end C;\n";
    // #5140: bare `Task` is now ambiguous (§2.3) — compile rejects.
    BEZEL_EXPECT_EQ(-1, buildAndRunWithLib(kLibWithUserTask, mainSrc));
}

// ── Part 7: module-qualified user `Task` (§2.7 rule 2) ──
BEZEL_TEST(task_completed_task_4977,
           module_qualified_user_task_static_property_is_unaffected) {
    const std::string mainSrc =
        "module M;\n"
        "import Lib;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return Lib.Task.Answer;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib(kLibWithUserTask, mainSrc));
}

// ── Part 8: alias-qualified user `Task` (§2.2 `Alias.Type`) ──
BEZEL_TEST(task_completed_task_4977,
           alias_qualified_user_task_static_property_is_unaffected) {
    const std::string mainSrc =
        "module M;\n"
        "import Lib as L;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return L.Task.Answer;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib(kLibWithUserTask, mainSrc));
}
