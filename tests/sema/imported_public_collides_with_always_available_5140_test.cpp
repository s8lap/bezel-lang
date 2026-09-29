// Regression test for issue #5140 (sema, v0.309 / §2.3, fix #5140).
//
// Spec §2.3 (v0.309, #5004, option a): "a same-named public exported by an
// explicitly imported module does not shadow [the always-available name]
// but makes the bare name **ambiguous** in that file under §2.2's collision
// rule — every row alike". The pre-fix compiler only enforced §2.2's
// same-named-IMPORTED-MODULES rule; an imported public named like an
// always-available type silently overrode the always-available one in
// moduleScope, so bare `BitReader` / `Task` / etc. resolved to the
// imported public and the always-available shape was unreachable (only
// qualification via the importing module reached it).
//
// Behavior the #5140 fix enforces:
//   * T1 / T3: a bare identifier named like an always-available type, when
//     an imported module's public has the same name, is now rejected as
//     ambiguous. The §2.3 fix is the source of the diagnostic; the per-fix
//#4977 closed-surface "no member" / "no static properties" arms may ALSO
//     fire on the same program, but those are secondary (they assume the
//     builtin Task was reached; with the bare name now ambiguous they
//     become noise rather than the spec verdict — the §2.3 diagnostic is
//     the one the test asserts).
//   * T2 / T4: qualification (`ModuleName.Name`) reaches the imported
//     public directly, and an imported public NOT colliding with any
//     always-available name still resolves by bare name as it did
//     pre-#5140.
//   * T5: a non-generic imported `Counter` coexists with the always-
//     available `Counter<K>` (per §2.3 Row 10 / stdlib §4.14). This row
//     is HELD pending the (identifier, generic arity) keying spec question
//     filed with #5140 — the implementation excludes `Counter` so the
//     held cross_module_import_test.cpp rows (374/399/422/445/469/834/
//     2710/2920/3612) and xmod_static_init_798_test.cpp:80 keep passing.
//     The shape here mirrors one of those held rows verbatim so the
//     pinned expected behaviour is part of this regression test's own
//     surface.
//
// runAot drives the strict AOT oracle: a fork-isolated Driver::compileFile
// + clang link + native exec. ASan is OFF in lanes by default (CLAUDE.md);
// this is pure sema so no generated-code sanitiser is required.

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

// ── Cross-module harness (mirrors task_completed_task_4977_test.cpp's
//    buildAndRunWithLib verbatim; the same pattern suffices — the #5140
//    rows only need a lib + main, link, run). ──
struct TempFile5140 {
    std::string path;
    explicit TempFile5140(std::string p) : path(std::move(p)) {}
    ~TempFile5140() { std::remove(path.c_str()); }
};

int buildAndRunWithLib(const std::string& libSrc, const std::string& mainSrc) {
    const std::filesystem::path& dir = wtScratchDir("wt-5140");
    const std::string stem = unique_stem("bezel_5140_");
    TempFile5140 libBzl((dir / (stem + "_lib.bzl")).string());
    TempFile5140 libObj((dir / (stem + "_lib.o")).string());
    TempFile5140 mainBzl((dir / (stem + "_main.bzl")).string());
    TempFile5140 exe((dir / stem).string());
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

// The lib shape that triggers the §2.3 imported-public-vs-always-available
// collision: a public class whose simple name is `Task`. The user's `Task`
// here is non-generic; the always-available `Task` (stdlib §6.7 / kBuiltin-
// TaskLike) accepts both arities.
const char* const kLibWithUserTask =
    "module Lib;\n"
    "public static class Task\n"
    "  public static readonly property Answer: int := 42;\n"
    "end Task;\n";

// A lib with an UNRELATED public name — does not collide with any §2.3
// always-available name. Pin: bare `Widget` from this lib resolves by §2.2
// as it did pre-#5140 (no new ambiguity introduced).
const char* const kLibWithUnrelatedPublic =
    "module Lib;\n"
    "public static class Widget\n"
    "  public static readonly property Value: int := 7;\n"
    "end Widget;\n";

// A lib exporting a NON-GENERIC `Counter` class. This is the held shape:
// per the issue's HOLD note, this commit does not key on name alone
// (pending the (identifier, generic arity) question), so the implementation
// excludes `Counter` from the ambiguity rule. The shape is the verbatim
// lib from cross_module_import_test.cpp:374 (the first of the ten held
// rows). The test asserts it still compiles and runs to 10 — pinning the
// HOLD.
const char* const kLibWithNonGenericCounter =
    "module Lib;\n"
    "public class Counter\n"
    "  public v: int;\n"
    "  public constructor(start: int) v := start; end constructor;\n"
    "  public method Get(): int => v;\n"
    "  destructor end destructor;\n"
"end Counter;\n";

}  // namespace

// ── T1: §2.3 ambiguous reject (the issue's #4977 example) ──
//
// `import Lib;` (Lib exports a public `Task`) and bare `Task.Answer` —
// pre-#5140 this resolved to Lib.Task (the imported public shadowed the
// always-available Task in moduleScope). Post-fix the bare name is
// ambiguous (§2.3) and the compile rejects. The compile-failure exit
// code (-1) is the verdict pin; the specific §2.3 phrasing in the
// diagnostic message is verified by the FAIL-WITHOUT probe against
// bezelc (which has a stdout path that exposes the compiler's
// diagnostic text) — pinning it inside runAot would require a Driver
// overload exposing the SemaContext's Diagnostics object, and the
// -1 verdict already captures the spec outcome (the program is
// rejected at compile time, not silently lowered to a degraded
// zero/null value as on origin/main before this fix).
BEZEL_TEST(imported_public_collides_with_always_available_5140,
           bare_imported_task_is_ambiguous_per_section_2_3) {
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

// ── T2: qualified reach to the imported public (§2.2 / §2.7 rule 2) ──
//
// `Lib.Task.Answer` reaches the imported public by qualification; each
// side of the §2.3 collision is reachable by qualification. This program
// must still compile and run to 42.
BEZEL_TEST(imported_public_collides_with_always_available_5140,
           module_qualified_imported_task_reaches_user_member) {
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

// ── T3: the always-available builtin is reachable via its §2.7-rule-2
//    module-qualified name even with a colliding import in scope ──
//
// Per spec §2.3 (v0.309, "every row alike"): when an explicitly imported
// module exports a public with the same identifier as an always-available
// type, the BARE name is ambiguous. Each side remains reachable by
// qualification — the imported one via `Lib.Task` (T2's pin), the
// always-available one via its module's qualified name where it has one
// (`System.Threading.Task`, §2.7 rule 2). For VALUE positions the
// qualified head already resolves (`Task.CompletedTask` per
// task_qualified_member_5006_test.cpp Part 1); this row pins that the
// always-available side still resolves for a CALL when the import-side
// has the same identifier.
//
// The "bare `Task<int>` is unusable in such a file" cost the spec
// states is intentionally NOT pinned here — qualifying an `async`
// method's return type as `System.Threading.Task<int>` is a separate,
// pre-existing gap (per task_qualified_member_5006_test.cpp's own
// `partialModulePaths_` chain: "the pseudo-module is never a real
// loaded artifact", so `ctx_.importedTypesOf("System.Threading")` is
// always null). The CALL-form pin below is what the spec verdict
// actually establishes; the type-form coverage is filed separately.
BEZEL_TEST(imported_public_collides_with_always_available_5140,
           module_qualified_builtin_task_reaches_always_available) {
    const std::string program =
        "module M;\n"
        "import Lib;\n"
        "import System.Threading;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    System.Threading.Task.FireAndForget(\n"
        "      System.Threading.Task.FromResult(42));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(0, r.exitCode);
}

// ── T4: an imported public NOT colliding with any always-available name
//    is unaffected by the fix (no false reject). ──
//
// The fix must not introduce false ambiguity for ordinary cross-module
// imports. A `Widget` (no §2.3 row collision) imported from `Lib` is
// reachable by bare name as it was pre-#5140 — no diagnostic.
BEZEL_TEST(imported_public_collides_with_always_available_5140,
           imported_public_not_colliding_is_unaffected) {
    const std::string mainSrc =
        "module M;\n"
        "import Lib;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    return Widget.Value;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(7, buildAndRunWithLib(kLibWithUnrelatedPublic, mainSrc));
}

// ── T5: HELD `Counter` rows are NOT flipped (per the issue's HOLD note) ──
//
// The companion spec question (whether the collision is keyed on
// (identifier, generic arity)) is open. Until it is answered the
// implementation excludes `Counter` from the ambiguity rule so the
// existing cross_module_import_test.cpp rows at lines 374 / 399 / 422 /
// 445 / 469 / 834 / 2710 / 2920 / 3612 and xmod_static_init_798_test.cpp:
// 80 keep passing. This test pins the held behaviour using the verbatim
// shape of cross_module_import_test.cpp:374's lib — bare `Counter` from
// the lib resolves to the imported public (constructor + Get()) and the
// program runs to 10.
//
// When the spec question is answered the implementation will need to be
// updated and this row's expectation flipped; the issue's HOLD note is
// the pin against accidentally doing so before then.
BEZEL_TEST(imported_public_collides_with_always_available_5140,
           held_counter_rows_are_not_flipped_pending_arity_ruling) {
    const std::string mainSrc =
        "module M;\n"
        "import Lib;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var c := new Counter(10);\n"
        "    return c.Get();\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(10, buildAndRunWithLib(kLibWithNonGenericCounter,
                                            mainSrc));
}