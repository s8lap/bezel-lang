// Spec-derived regression test for issue #3747 — the GENERIC-import twin of
// fix #3731. `emitImplicitBase` (src/codegen/LlvmBackend_Setup.cpp) binds the
// §22.10 inheritance-clause base call to a base's PRIMARY constructor when
// the base ALSO declares an explicit constructor. #3731 fixed this for a
// LOCAL base and for an IMPORTED NON-GENERIC base (routed through
// `bz$<Base>$initprimary`, #3713's entry point), but deliberately EXCLUDED an
// IMPORTED **generic** base from the new arm with the guard
// `(!ctx_.isImported(b) || b->genericParams.empty())`, because a generic
// class has no exported `$initprimary` (the exporter's `initFns_` gate
// requires `genericParams.empty()` — no entry point exists to call).
//
// ── THE SHAPE ──────────────────────────────────────────────────────────────
//
//     public class Base<T>(bp: int)
//       public bx: int := 11;
//       public constructor()
//         this(0);
//       end constructor;
//     end Base;
//     class Derived(a: int): Base<int>(a)
//     end Derived;                          // Derived is LOCAL, Base<T> is
//                                            // IMPORTED and GENERIC
//
//     new Derived(7);   // the base clause `Base<int>(a)` must bind to
//                       // Base's PRIMARY constructor (arity 1) -- Base's
//                       // only EXPLICIT constructor has arity 0 and does not
//                       // match these arguments.
//
// ── WHAT THE DEFECT WAS (measured on r14/c14-l3, before this fix) ──────────
//
// Excluded by the `b->genericParams.empty()` guard, this shape fell through
// to the general fallback at the bottom of `emitImplicitBase`
// (`ctorForArgs(b, emptyArgs, nullptr)`), which resolves an IMPLICIT
// `base()` call — i.e. treats the inheritance clause as if it supplied ZERO
// arguments. Base's explicit `constructor()` has exactly arity 0, so this
// fallback finds it and calls it with NO arguments, silently dropping the
// value the base clause was supposed to bind to the primary. That explicit
// constructor's own `this(0)` chain then re-runs the primary's parameter
// storage with ITS OWN literal, clobbering the value
// `prePopulateBasePositionalFields` had already stored.
//
// MEASURED (bezelc @ r14-c14-l3 pre-fix, this lane): `a=7 bp=0 bx=11
// ctorRuns=1 fieldInitRuns=1` — `bp` wrong (dropped), and `ctorRuns=1` proves
// the explicit (zero-arg) constructor body actually ran, which is exactly
// the wrong path per §22.10.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// The `b->genericParams.empty()` half of the exclusion guard is removed.
// `ctx_.isImported(b)` was never the right axis for this specific step: an
// imported type NEVER carries an `ast::TypeDecl` (`TypeSymbol::decl` is null
// for every imported type, generic or not — see ModuleLoader.cpp's TypeSymbol
// shell comment, the #2374-class gap), so "IMPORTED" alone does not mean "no
// local AST to replay". A GENERIC import is different from a NON-generic one
// precisely because ModuleLoader's pass 3 (§14/#662c, #2860/#2874) already
// reconstructs `b`'s member-level AST (`ast::ConstructorDecl` bodies via
// `ctorBodyText`, field-initializer text via `fieldInitText`) so it can be
// replayed under a per-instantiation generic substitution — the SAME
// machinery `emitImportedBaseFieldInit`'s pre-existing #2524 generic arm
// already uses for the sibling NO-EXPLICIT-CTOR shape. So the fix routes an
// imported GENERIC base with both constructor forms through the SAME two
// calls the LOCAL arm already makes (`emitImplicitBase` + IMPORTED NON-
// GENERIC arm) already makes for a LOCAL base:
//
//     emitImplicitBase(b, obj, mostDerived);
//     emitLocalBaseFieldInit(b, obj, mostDerived);
//
// `emitLocalBaseFieldInit` computes `computeAncestorGenericSubst` from
// `mostDerived` itself before running `b`'s field initializers, so it is
// already generic-substitution-aware — reused unchanged, not duplicated.
// `primaryInitFn` (the `bz$<Base>$initprimary` call) stays reserved for the
// IMPORTED NON-GENERIC case, since that is the only shape it is exported for.
//
// per §22.8 step 1 "The base or chained constructor call", issue #3747
// per §22.10 (v0.294) — "This replaces an explicit `base(...)` call"
// per §2.8.3 rule 4 (v0.294) — the importer observes the same surface
// per fix #3731 — the non-generic twin this issue follows up on
// per fix #2524 — the no-explicit-ctor imported-generic precedent reused here
// per #3162 — the base-clause-argument asymmetry family

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base — forked
// shards would otherwise race on one another's `.o` in /tmp (tmpfs, RAM).
const std::filesystem::path& wtDir3747() {
    return wtScratchDir("wt-3747-codegen");
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

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The dependency object. Its `TempFile` MUST outlive the dependent compile —
// `Driver::compileFile` reopens the `.o` to read its `.bezel.meta` section.
struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::string& libSrc) {
    LibHandle h;
    auto dir = wtDir3747();
    std::string stem = unique_stem("bezel_3747lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct AppHandle {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

AppHandle buildApp(const std::string& libObjPath, const std::string& appSrc) {
    AppHandle r;
    if (libObjPath.empty()) return r;
    auto dir = wtDir3747();
    std::string stem = unique_stem("bezel_3747app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    // compileFileForTest, not a raw driver.compileFile call, so this runner
    // is reached by the gate's `--full` generated-code ASan sweep
    // (support/driver_run_support.hpp) like every other hand-rolled runner.
    bezel::CompileResult rb =
        bezel::test::compileFileForTest(driver, r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

// The base hierarchy shared by the headline rows: a GENERIC primary
// constructor (arity 1, `bp`) AND an explicit constructor of a DIFFERENT
// arity (0) — the exact shape #3747 is about, one level up an import
// boundary from #3731's non-generic twin. `bx` pins that Base's own field
// initializer (§22.8 step 2) still ran.
const char* kGenericBaseBothForms =
    "public class Base<T>(bp: int) "
    "  public bx: int := 11; "
    "  public constructor() "
    "    this(0); "
    "  end constructor; "
    "end Base; ";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// T1 — IMPORTED GENERIC base, LOCAL derived: the headline #3747 shape.
// `Base<T>` (both forms) lives in a dependency and is instantiated as
// `Base<int>` at the (local) derived class's base clause; `Derived` is
// declared in the IMPORTER and is not itself exported/imported.
// FAIL-WITHOUT (measured, this lane): prints "a=7 bp=0 bx=11" — identical
// failure mode to #3731's T1/T2, reached through the newly-excluded generic
// arm instead of the non-generic one.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10 / §2.8.3 rule 4, issue #3747
BEZEL_TEST(base_clause_primary_ctor_both_forms_generic_import_3747,
           imported_generic_base_both_forms_binds_primary_spec_22_8) {
    std::string lib = std::string("module XL3747a; ") + kGenericBaseBothForms;
    const std::string app =
        "module XM3747a; "
        "import XL3747a; "
        "class Derived(a: int): Base<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=7 bx=11") != std::string::npos);
    // The pre-fix line, excluded explicitly so a regression cannot be read as
    // "some other value happened to contain the expected substring".
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=0 bx=11") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T2 — THE COUNTERS, LITERALLY (the review's own language: "exactly one base
// init, zero explicit-body runs"). A companion non-generic `Counters` class
// counts field-initializer runs and explicit-constructor-body runs
// independently of the `bp` value, so a wrong-but-coincidentally-7 result
// cannot masquerade as correct: `ctorRuns` pins the explicit zero-arg
// constructor NEVER ran (its body would otherwise increment it once per
// construction), and `fieldInitRuns` pins Base's own field initializer ran
// EXACTLY once (not zero, not twice via a double dispatch).
// FAIL-WITHOUT (measured, this lane): prints "... ctorRuns=1 fieldInitRuns=1"
// — the explicit constructor's body actually ran (the wrong path), on top of
// `bp` being dropped.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3747 (review requirement: exactly one
// base init, zero explicit-body runs)
BEZEL_TEST(base_clause_primary_ctor_both_forms_generic_import_3747,
           exactly_one_base_init_zero_explicit_ctor_runs_counters) {
    const std::string lib =
        "module XL3747b; "
        "public class Counters "
        "  public static ctorRuns: int := 0; "
        "  public static fieldInitRuns: int := 0; "
        "  public static method MarkFieldInit(): int "
        "    Counters.fieldInitRuns := Counters.fieldInitRuns + 1; "
        "    return 11; "
        "  end MarkFieldInit; "
        "end Counters; "
        "public class Base<T>(bp: int) "
        "  public bx: int := Counters.MarkFieldInit(); "
        "  public constructor() "
        "    this(0); "
        "    Counters.ctorRuns := Counters.ctorRuns + 1; "
        "  end constructor; "
        "end Base; ";
    const std::string app =
        "module XM3747b; "
        "import XL3747b; "
        "class Derived(a: int): Base<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx} "
        "ctorRuns={Counters.ctorRuns} fieldInitRuns={Counters.fieldInitRuns}\"); "
        "  end Show; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(
        out.find("a=7 bp=7 bx=11 ctorRuns=0 fieldInitRuns=1") !=
        std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("ctorRuns=1") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T3 — LOCAL generic base, both forms (REGRESSION CONTROL). This shape was
// ALREADY routed through the "route to primary" arm before this fix — the
// excluded guard was `(!ctx_.isImported(b) || b->genericParams.empty())`,
// which already admitted a non-imported (LOCAL) generic base. Confirms the
// guard simplification (dropping the generic half entirely and dispatching
// on `ctx_.isImported(b) && b->genericParams.empty()` INSIDE the arm instead)
// does not perturb the pre-existing LOCAL-generic behavior.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / §22.10, issue #3747 (regression control)
BEZEL_TEST(base_clause_primary_ctor_both_forms_generic_import_3747,
           control_local_generic_base_both_forms_unchanged) {
    const std::string src =
        std::string("module L3747c; ") + kGenericBaseBothForms +
        "class Derived(a: int): Base<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("a=7 bp=7 bx=11") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T4 — THE "SAME ARITY" PIN (§6b), IMPORTED GENERIC twin of #3731's T5: base
// declares a GENERIC primary constructor AND an explicit constructor of the
// SAME arity as the base clause's argument count. Per §22.10 ("This replaces
// an explicit `base(...)` call"), the EXPLICIT constructor wins —
// `ctorForArgs` finds it directly (an exact arity+type match), so the "route
// to primary" branch never fires and this fix's guard change is provably
// INERT for this shape. Distinguishable from routing to the primary because
// the explicit ctor multiplies its argument by 100 before chaining.
// FAIL-WITHOUT is not meaningful here (this shape reaches the SAME `if`
// branch before and after #3747) — its purpose is to pin the resolution rule
// under an import boundary so a future change cannot silently flip it.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.10 "This replaces an explicit `base(...)` call", issue #3747
BEZEL_TEST(base_clause_primary_ctor_both_forms_generic_import_3747,
           same_arity_explicit_ctor_wins_imported_generic_spec_22_10) {
    const std::string lib =
        "module XL3747d; "
        "public class Base<T>(bp: int) "
        "  public bx: int := 11; "
        "  public constructor(v: int) "
        "    this(v * 100); "
        "  end constructor; "
        "end Base; ";
    const std::string app =
        "module XM3747d; "
        "import XL3747d; "
        "class Derived(a: int): Base<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    // The explicit constructor ran (bp = 7*100 = 700), not the primary
    // directly (which would read bp = 7).
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=700 bx=11") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.find("bp=7 ") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// T5 — OVER-ACCEPTANCE / REGRESSION CONTROL: IMPORTED GENERIC base has a
// primary constructor and NO explicit constructor at all. This shape is the
// PRE-EXISTING `emitImportedBaseFieldInit` #2524 generic arm (unconditional
// on `hasAnyConstructor(b) == false`), untouched by this fix (this fix's arm
// requires `hasAnyConstructor(b) == true`). Must stay byte-identical.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.8 step 1 / fix #2524, issue #3747 (regression control)
BEZEL_TEST(base_clause_primary_ctor_both_forms_generic_import_3747,
           control_imported_generic_primary_only_no_explicit_ctor_unchanged) {
    const std::string lib =
        "module XL3747e; "
        "public class Base<T>(bp: int) "
        "  public bx: int := 11; "
        "end Base; ";
    const std::string app =
        "module XM3747e; "
        "import XL3747e; "
        "class Derived(a: int): Base<int>(a) "
        "  public method Show() "
        "    Console.WriteLine($\"a={a} bp={bp} bx={bx}\"); "
        "  end Show; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(7); "
        "    d.Show(); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle libH = buildLib(lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    AppHandle a = buildApp(libH.objPath, app);
    BEZEL_EXPECT_TRUE(a.ok);
    if (!a.ok) return;
    auto [code, out] = runCapture(a.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_TRUE(out.find("a=7 bp=7 bx=11") != std::string::npos);
}

}  // namespace bezel::codegen
