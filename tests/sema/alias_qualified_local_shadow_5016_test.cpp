// Regression test for issue #5016 (sema, spec §2.7 rule 1, v0.307).
//
// THE GOVERNING RULE. Spec §2.7 rule 1: "Local declarations, then enclosing-
// type members, then module declarations, then imported bare names, then
// aliases" — i.e. a local/parameter/field with the same spelling as an import
// alias outranks the alias for chain-head resolution. The alias becomes
// unreachable through the chain-head position (the local wins).
//
// ROOT CAUSE. `ExprTC::resolveMemberAccess`'s Case-(a) alias arm
// (`src/sema/ExprTypecheck_Calls.cpp`, ~L20111) matched `importAliases_` by
// NAME only, with no check for whether the chain-head identifier had been
// shadowed by a local/parameter/field in the current scope. Pre-fix repro
// (`T.Widget` with `import OtherMod2 as T;` and a local `T := w;`):
//   1. The Case-(a) arm fires unconditionally, finds `T` → `OtherMod2`,
//      returns the imported `Widget` *type* (OtherMod2.Widget is a class).
//   2. The subsequent `return T.Widget;` then §5.1-fails — the type doesn't
//      match int — producing a misleading type-mismatch diagnostic that
//      hides the actual cause (a local shadowed the alias).
// MEASURED pre-fix: rejects with §5.1 type-mismatch (the wrong message).
//
// FIX. Guard the Case-(a) alias lookup with `!lookupName(iid->name)`,
// mirroring the identical precedent already used a few lines below by the
// pre-existing Case-(b) module-qualified-path-extension code
// (`partialModulePaths_` construction at ~L20151, originally added for an
// analogous local-named-`System` shadow case) and the alias-qualified Task
// gate (#5006 at ~L20867, where the same shadow bug appeared for the
// closed-Task surface). `iid` is trusted as naming the alias ONLY when it
// does not already resolve to a real scope symbol, so a local/parameter/
// field of the same spelling makes the alias arm fall through and the
// ordinary member-lookup path on the local runs.
//
// CROSS-MODULE HARNESS. The bug ONLY manifests when the alias's target
// module actually exports the requested member (`importedTypesOf("T")`
// is null in a single-file test, so the alias arm's inner `for` loop
// never executes and the fall-through behaviour is identical with or
// without the fix). All five rows therefore drive a real
// compileObject-of-OtherMod2 + compileFile-of-main pattern, the same
// one `task_qualified_member_5006_test.cpp` uses for the related #5006
// harness, so OtherMod2.Widget is genuinely exported as a class type
// when the alias arm fires.
//
// PARTS 1-4 below pin the bug and its boundaries (local, parameter, field,
// declared-after), each asserting a runtime VALUE (not just `r.ran`) so the
// test cannot pass by silently degrading to a zero-initialized discard —
// the same "VALUE assertion, not just compile-success" discipline as
// #4977/#5006 Parts 4, 5, 7, 8. Part 5 is the accept control (no shadow,
// alias still wins), guarding against an over-correction that would
// reject the no-shadow case.

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

// Cross-module harness, mirroring task_qualified_member_5006_test.cpp's
// `buildAndRunWithLib5006`: compile `libSrc` to a library object, compile
// `mainSrc` against it as a dependency (§2.5), link, run, and return the
// exit code — -1 on any compile, link or run failure. The alias target
// (`OtherMod2.Widget`, a class with an `N` property) must live in a
// separate module because aliases only resolve across module boundaries,
// AND the bug only manifests when OtherMod2's exports are actually loaded
// (a single-file test sees `importedTypesOf("OtherMod2") == nullptr` and
// the alias arm's inner loop never runs — so the fall-through path
// masks the bug entirely).
int buildAndRunWithLib5016(const std::string& libSrc,
                            const std::string& mainSrc) {
    const std::filesystem::path& dir = wtScratchDir("wt-5016");
    const std::string stem = unique_stem("bezel_5016_");
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

// The shared library side: `OtherMod2` exports a `Widget` class with an
// instance property `N` and a SECOND class `Gizmo` with a property `V`
// for cases where the local's `Widget` member is the unrelated value
// (so `T.Widget` through the alias returns the OtherMod2.Widget TYPE,
// not the local's int — a §5.1 type mismatch). `T.Gizmo` exercises the
// alias path further when no local shadow exists.
const std::string kLibWithWidget =
    "module OtherMod2;\n"
    "public class Widget\n"
    "  public readonly property N: int := 99;\n"
    "end Widget;\n"
    "public class Gizmo\n"
    "  public readonly property V: int := 7;\n"
    "end Gizmo;\n";

}  // namespace

// ── Part 1: local shadows alias — alias arm must NOT fire (§2.7 rule 1) ──
//
// `import OtherMod2 as T; var T := w;` (where `w` is a `Wrapper` whose own
// `Widget` property is an unrelated `int`, NOT OtherMod2.Widget's class):
// `T.Widget` must resolve as an ordinary property read on the LOCAL `T`,
// returning 42 (the Wrapper's Widget value). MEASURED pre-fix (cross-module
// harness): rejected with §5.1 type mismatch, because the alias arm returned
// OtherMod2's `Widget` *type* (a class) instead of Wrapper's `Widget` int
// property. Asserts the runtime VALUE (42), not merely `r.ran`.
BEZEL_TEST(alias_qualified_local_shadow_5016,
           local_shadows_alias_resolves_via_local_field) {
    const std::string libSrc = kLibWithWidget;
    const std::string mainSrc =
        "module M;\n"
        "import OtherMod2 as T;\n"
        "class Wrapper\n"
        "  public readonly property Widget: int := 42;\n"
        "  public constructor() end constructor;\n"
        "end Wrapper;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var w := new Wrapper();\n"
        "    var T := w;\n"
        "    return T.Widget;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib5016(libSrc, mainSrc));
}

// ── Part 2: parameter shadows alias — alias arm must NOT fire (§2.7 rule 1) ──
//
// Same shape as Part 1, but the shadowing symbol is a METHOD PARAMETER
// (`T: Wrapper`) instead of a local. Per §2.7 rule 1 a parameter outranks
// an alias too; the alias arm must skip and ordinary member-lookup runs
// on the parameter. Returns 42 (parameter.Widget).
BEZEL_TEST(alias_qualified_local_shadow_5016,
           parameter_shadows_alias_resolves_via_parameter_field) {
    const std::string libSrc = kLibWithWidget;
    const std::string mainSrc =
        "module M;\n"
        "import OtherMod2 as T;\n"
        "class Wrapper\n"
        "  public readonly property Widget: int := 42;\n"
        "  public constructor() end constructor;\n"
        "end Wrapper;\n"
        "class C\n"
        "  public static method Use(T: Wrapper): int\n"
        "    return T.Widget;\n"
        "  end Use;\n"
        "  public static method Main(): int\n"
        "    return Use(new Wrapper());\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib5016(libSrc, mainSrc));
}

// ── Part 3: implicit field shadows alias — alias arm must NOT fire
//    (§2.7 rule 1) ──
//
// A class `C` declares an instance field `T: Wrapper`. Inside an
// instance method `Use`, the bare identifier `T` resolves as the
// implicit `this.T` field per §2.7 rule 1's enclosing-type-member
// step. `T.Widget` is then a MemberAccess on that field, so the alias
// arm's `iid->name == "T"` matches and would fire UNLESS the
// `lookupName` guard rejects it. Pre-fix: alias arm fires, T.Widget
// resolves to OtherMod2.Widget (class), `return T.Widget` (§5.1 type
// mismatch — class can't be returned as int). Post-fix: lookupName
// finds the field, alias arm skipped, regular member-lookup returns
// Wrapper's int property (42).
BEZEL_TEST(alias_qualified_local_shadow_5016,
           implicit_field_shadows_alias_resolves_via_field) {
    const std::string libSrc = kLibWithWidget;
    const std::string mainSrc =
        "module M;\n"
        "import OtherMod2 as T;\n"
        "class Wrapper\n"
        "  public readonly property Widget: int := 42;\n"
        "  public constructor() end constructor;\n"
        "end Wrapper;\n"
        "class C\n"
        "  public owned T: Wrapper := new Wrapper();\n"
        "  public method Use(): int\n"
        "    return T.Widget;\n"
        "  end Use;\n"
        "  public static method Main(): int\n"
        "    var c := new C();\n"
        "    return c.Use();\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(42, buildAndRunWithLib5016(libSrc, mainSrc));
}

// ── Part 4: declared-after local does NOT shadow — alias still wins ──
//
// A local declared AFTER its use cannot shadow — the symbol table at the
// use site has no entry for it. `var w := new T.Widget(); var T := w;` —
// the `new T.Widget()` line sees no `T` in scope, so the alias arm fires
// (T = OtherMod2, T.Widget = OtherMod2.Widget class), `w` becomes a
// Widget instance, and `w.N` returns 99. Pre-fix: same result. Post-fix:
// same result (the guard does not affect this case — `lookupName("T")`
// at the use site returns null, so the guard is satisfied). This pins
// that the fix does NOT over-correct: a local declared after the use
// does NOT shadow the alias.
BEZEL_TEST(alias_qualified_local_shadow_5016,
           declared_after_local_does_not_shadow_alias) {
    const std::string libSrc = kLibWithWidget;
    const std::string mainSrc =
        "module M;\n"
        "import OtherMod2 as T;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var w := new T.Widget();\n"
        "    var T: int := 0;\n"
        "    return w.N;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(99, buildAndRunWithLib5016(libSrc, mainSrc));
}

// ── Part 5: accept control — no local at all, alias wins (§2.7 rule 1
//    boundary: no shadow, alias path is the correct path) ──
//
// `import OtherMod2 as T; var w := new T.Widget(); return w.N;` — no
// local/parameter/field named `T` shadows the alias, so the alias arm
// fires (`lookupName("T")` returns null), `T.Widget` resolves via the
// alias to OtherMod2.Widget (a class), `new T.Widget()` produces an
// instance, `w.N` returns 99. Pre-fix and post-fix: both pass with 99.
// Pins that the fix does NOT regress the alias-wins path when nothing
// shadows.
BEZEL_TEST(alias_qualified_local_shadow_5016,
           no_shadow_alias_still_wins) {
    const std::string libSrc = kLibWithWidget;
    const std::string mainSrc =
        "module M;\n"
        "import OtherMod2 as T;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var w := new T.Widget();\n"
        "    return w.N;\n"
        "  end Main;\n"
        "end C;\n";
    BEZEL_EXPECT_EQ(99, buildAndRunWithLib5016(libSrc, mainSrc));
}
