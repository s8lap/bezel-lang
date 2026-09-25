// fix #4039 (§9.1 "Records" / §9.3 "Combined form", v0.294): a COMBINED-FORM
// record (positional header fields, §9.1, plus a body, §9.3) that ALSO
// declares an EXPLICIT secondary constructor of a DIFFERENT arity made its
// own auto-generated positional constructor silently unconstructible at
// codegen.
//
// ROOT CAUSE: `Emitter::hasAnyConstructor` (LlvmBackend_Setup.cpp) returns
// true the moment a type declares ANY non-static explicit constructor, with
// no arity check of its own. `Emitter::buildRecordValue`'s §9.1 positional
// fast path (LlvmBackend_Exprs2.cpp) was gated `!hasAnyConstructor(t) && …`,
// so the fast path was blanket-disabled for a record with an explicit ctor —
// even when that call's arity matches the POSITIONAL fields, not the
// explicit ctor. `ctorForArgs(t, args)` then (correctly) finds no EXPLICIT
// match either, since the explicit ctor's arity differs from the call's —
// so BOTH of `buildRecordValue`'s construction paths were skipped and the
// function fell to its final `else if (!args.empty())` arm, which returned a
// bare `nullptr`. The caller, `lowerLocalDecl`'s pre-existing #2434 "failed
// to lower to a value" fallback, cannot tell this apart from the SIX
// unrelated, genuinely-permissive-typing shapes that fallback exists to
// tolerate (§3/#605, `where T: new()`), so it fired its `Severity::Warning`
// and degraded the local's slot to zero — compile EXIT 0, followed by a
// runtime SIGSEGV on the resulting garbage value (the field that was never
// stored still read as a null/zero reference, dereferenced on first use).
//
// Per §9.3, "[t]he auto-generated members are present and the body adds
// further members" — the only listed constructor conflict is a body
// constructor sharing the primary's EXACT parameter list — so a different-
// arity secondary constructor does not retire the record's own
// auto-generated primary constructor, and `new Combo(5)` below (or any call
// whose arity matches the positional fields) is legal §9.1/§9.3 source with
// no reachable construction path in the pre-fix implementation, not a call
// this function should silently drop.
//
// THE FIX has two parts, both in `Emitter::buildRecordValue`
// (LlvmBackend_Exprs2.cpp):
//
//   (a) The §9.1 fast-path gate is widened to also admit a call whose arity
//       matches the record's POSITIONAL fields (`sema::
//       recordPositionalFieldSymbols`) when `hasAnyConstructor(t)` is true,
//       PROVIDED (i) no explicit constructor actually claims the call
//       (`ctorForArgs(t, args) == nullptr`) and (ii) the record's TOTAL
//       storage equals its positional-field count — i.e. there is no body
//       field/auto-property this path could leave uninitialized. Condition
//       (ii) is deliberately conservative: it makes the widening provably
//       sound WITHOUT depending on the record's own §7.9 field-completion
//       checking, a separate, still-open gap tracked as issue #4001 (a
//       record body field/auto-property with no initializer is never
//       checked for definite assignment at all — #4001, not fixed here).
//       T3-T8 below exercise this accept-side fix; every expected value is
//       an ADDITION of distinct components so it cannot arise by coincidence
//       from a masked exit code or a dropped field (`runAot`'s exitCode is
//       WEXITSTATUS-masked to 8 bits, so every sum here stays under 256).
//
//   (b) The remaining, genuinely-unreachable case (condition (ii) above does
//       NOT hold — the record has body storage beyond its positional
//       fields, exactly the ORIGINAL #4039 repro with an unassigned `Extra`
//       field) still reaches the final `else if (!args.empty())` arm, which
//       now reports a LOCATED `Severity::Error` — "no constructor of `T`
//       accepts them" — instead of silently returning `nullptr` into the
//       generic #2434 Warning path. T9 pins this as a compile-time
//       rejection, not a crash (`failedAtCompileTimeWithRealLocation`,
//       issue #3892's remedy for exactly this "cannot pin exact message
//       text everywhere, but MUST distinguish reject from crash" shape).
//
// UPDATE — fix #4058 (§9.1/§9.3/§7.9, v0.295): condition (ii) above ("record's
// TOTAL storage equals its positional-field count", i.e. ZERO body storage)
// was a strict subset of what is actually sound. The preamble field-init
// loop that runs unconditionally before this gate already correctly
// initializes any body field/auto-property that carries ITS OWN
// initializer, regardless of which construction path follows below it — so
// such a member is not "storage this path could leave uninitialized" after
// all. #4058 widens condition (ii) to: every body-storage member either is
// positional, or has a real local initializer the preamble loop will run
// (an IMPORTED body member with no local decl stays conservative, i.e.
// still declines — a separate concern, #2467/#3671/#3723). T8 below was
// originally a control pinning this exact shape as a decline (see git
// history); #4058 flips it to an accept-and-runs-correctly assertion, since
// the shape genuinely IS sound. T9's shape (a body field with NO
// initializer at all) is unaffected and still correctly declines — #4001
// remains the open general field-completion gap.
//
// Related but DISTINCT: issue #4017 (a combined-form record's explicit
// secondary constructor of a different arity is REJECTED by SEMA when the
// call's arity matches THAT constructor, not the positional fields —
// `checkNewExprArgs`'s record branch validates every `new` against
// `td->positionalFields` unconditionally and never considers the record's
// own explicit constructors as overload candidates, unlike the analogous
// CLASS branch). #4017 is a SEMA over-reject; #4039 is a CODEGEN silent
// mis-compile of a call sema currently (if arguably by the same underlying
// gap, from the other side) accepts. T10 pins the current (still open,
// #4017) sema-side boundary this fix does NOT touch: calling the record's
// OWN explicit constructor directly, at its own (different) arity, remains
// wrongly rejected before codegen ever runs.
//
// Both the LOCAL axis (T3-T10) and the CROSS-MODULE axis (T11, importing a
// combined-form record with a different-arity secondary constructor from a
// separate module) are covered — issue #4022 notes that a record `this(...)`
// chain-detection fix landed AST-gated and so was local-axis-only; this
// fix's predicates (`hasAnyConstructor`, `ctorForArgs`,
// `sema::recordPositionalFieldSymbols`) all read `sema::TypeSymbol` state
// populated identically for an imported type by `ModuleLoader`, so no such
// gap is expected here, and T11 measures it rather than assuming it.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"                // bezel::Driver / Dependency
#include "support/aot_result_asserts.hpp"   // failedAtCompileTimeWithRealLocation (#3892/#3895)
#include "support/aot_support.hpp"          // runAot — AOT test harness
#include "support/driver_run_support.hpp"   // compileFileForTest (ASan sweep, #3010-class)
#include "support/scratch_dir.hpp"          // wtScratchDir
#include "support/unique_stem.hpp"          // unique_stem
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// ── T11's minimal cross-module runner ───────────────────────────────────
// Mirrors the established per-file pattern in
// tests/codegen/xmod_record_destructure_pattern_over_imported_2844_test.cpp
// (buildLib/buildApp/runCapture): `runAot` is single-module, so a two-module
// import shape needs `Driver::compileObject` for the library + `Driver::
// compileFile` (via `compileFileForTest` for the ASan sweep, driver_run_
// support.hpp) with a `Dependency` for the importer, plus an out-of-process
// run to get a real exit code.
struct TempFile4039 {
    std::string path;
    explicit TempFile4039(std::string p) : path(std::move(p)) {}
    ~TempFile4039() { std::remove(path.c_str()); }
};

void writeSource4039(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult4039 { int code; };

RunResult4039 runCapture4039(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code};
}

// Returns the executable's exit code, or -999 on any compile failure.
int runXmod4039(const std::string& libSrc, const std::string& appSrc) {
    const auto& dir = wtScratchDir("wt-4039");
    std::string libStem = unique_stem("bezel_4039lib_");
    TempFile4039 libSrcFile((dir / (libStem + ".bzl")).string());
    TempFile4039 libObjFile((dir / (libStem + ".o")).string());
    writeSource4039(libSrcFile.path, libSrc);
    bezel::Driver libDriver;
    bezel::CompileResult libR =
        libDriver.compileObject(libSrcFile.path, libObjFile.path);
    if (!libR.success) return -999;

    std::string appStem = unique_stem("bezel_4039app_");
    TempFile4039 appSrcFile((dir / (appStem + ".bzl")).string());
    TempFile4039 appExeFile((dir / appStem).string());
    writeSource4039(appSrcFile.path, appSrc);
    bezel::Driver appDriver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjFile.path;
    d.objectPath = libObjFile.path;
    deps.push_back(d);
    bezel::CompileResult appR = bezel::test::compileFileForTest(
        appDriver, appSrcFile.path, appExeFile.path, deps);
    if (!appR.success) return -999;

    return runCapture4039(appExeFile.path).code;
}
}  // namespace

// ===========================================================================
// T1 — CONTROL: pure positional record, NO explicit constructor at all
// (`!hasAnyConstructor(t)` — the pre-existing, untouched arm of the gate).
// Must keep working exactly as before this fix.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           positional_only_record_no_explicit_ctor_control_spec_9_1) {
    BEZEL_EXPECT_EQ(
        run("module M; record Plain4039(x: int, y: int); "
            "class C public static method Main(): int "
            "  var p := new Plain4039(3, 4); return p.x + p.y; "
            "end Main; end C;"),
        7);
}

// ===========================================================================
// T2 — CONTROL: combined-form record whose explicit constructor MATCHES the
// call via `ctorForArgs` — must still route through that EXPLICIT
// constructor (the new fast-path disjunct's `ctorForArgs(t, args) ==
// nullptr` conjunct is false, so this path is untouched by the fix). The
// explicit ctor is `constructor(v: int?)` — arity 1, same as the positional
// field, but a DIFFERENT parameter list (`int?`, not `int`), which §9.3
// permits (only an IDENTICAL parameter list to the auto-generated primary
// constructor is forbidden). The argument is an `int?` local, so the explicit
// ctor is the EXACT candidate and the primary `(x: int)` is inapplicable
// (`int?` does not implicitly convert to `int`, §5.1). fix #4853 (§22.6 stage
// 5, v0.307): this row used to pass a bare `5`, for which the primary is the
// exact candidate and the `int?` ctor merely converting — so §22.6 selects
// the PRIMARY there (value 5, pinned in
// tests/sema/ctor_overload_exact_match_ranking_4853_test.cpp). The old
// expectation (50) came from `ctorForArgs`'s type-class proxy treating `int`
// and `int?` as one class (fix #3760), not from the spec.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           explicit_ctor_matching_ctorforargs_still_used_control_spec_9_3) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T2(x: int) "
            "  public constructor(v: int?) this.x := (v ?? 0) * 10; end constructor; "
            "end Combo4039T2; "
            "class C public static method Main(): int "
            "  var n: int? := 5; var c := new Combo4039T2(n); return c.x; "
            "end Main; end C;"),
        50);
}

// ===========================================================================
// T3 — THE #4039 FIX, minimal shape: ONE positional field, an explicit
// constructor of a DIFFERENT arity, NO body storage beyond the positional
// field. `new R(9)` (arity 1, matching the positional field, NOT the 2-arg
// explicit ctor) must now construct via the auto-generated primary
// constructor and return the stored value — asserts the RUNTIME VALUE, not
// merely that it compiles.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           different_arity_explicit_ctor_no_body_field_uses_primary_spec_9_1) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T3(x: int) "
            "  public constructor(x: int, y: int) this.x := x + y; end constructor; "
            "end Combo4039T3; "
            "class C public static method Main(): int "
            "  var c := new Combo4039T3(9); return c.x; "
            "end Main; end C;"),
        9);
}

// ===========================================================================
// T4 — same shape as T3, but the explicit ctor's arity happens to COINCIDE
// with the positional field's own arity while carrying a DIFFERENT parameter
// TYPE — a still-legal §9.3 combined form (only an identical PARAMETER LIST
// is forbidden). The auto ctor and the explicit ctor are candidates of the
// SAME arity; `ctorForArgs` must reject the explicit ctor on a TYPE mismatch
// (an `int` argument against a `string` parameter) so the call still falls
// through to the (now-reachable) primary constructor.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           same_arity_different_param_type_explicit_ctor_uses_primary_spec_9_3) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T4(x: int) "
            "  public constructor(s: string) this.x := -1; end constructor; "
            "end Combo4039T4; "
            "class C public static method Main(): int "
            "  var c := new Combo4039T4(9); return c.x; "
            "end Main; end C;"),
        9);
}

// ===========================================================================
// T5 — TWO positional fields, a THREE-arg explicit constructor, no body
// storage. Exercises the general `posFields = fields.end() - args.size()`
// slice with args.size() == 2 (not 1) alongside a wider-arity explicit ctor.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           two_positional_fields_three_arg_explicit_ctor_uses_primary_spec_9_1) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T5(x: int, y: int) "
            "  public constructor(a: int, b: int, c: int) "
            "    this.x := a; this.y := b + c; "
            "  end constructor; "
            "end Combo4039T5; "
            "class C public static method Main(): int "
            "  var r := new Combo4039T5(3, 4); return r.x * 10 + r.y; "
            "end Main; end C;"),
        34);
}

// ===========================================================================
// T6 — MULTIPLE explicit constructors, neither matching the positional
// arity, no body storage. Both candidates fail `ctorForArgs`; the primary
// constructor is still reachable.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           multiple_explicit_ctors_none_matching_uses_primary_spec_9_1) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T6(x: int) "
            "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
            "  public constructor(a: int, b: int, c: int) this.x := a + b + c; end constructor; "
            "end Combo4039T6; "
            "class C public static method Main(): int "
            "  var r := new Combo4039T6(9); return r.x; "
            "end Main; end C;"),
        9);
}

// ===========================================================================
// T7 — §22.5 NAMED argument to the auto-generated primary constructor still
// works once the fast path is reachable (the fast path's own named-argument
// reordering, `allPositional`/field-name matching, is pre-existing and
// unaffected by this fix's gate widening).
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           named_argument_to_primary_ctor_spec_22_5) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T7(x: int) "
            "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
            "end Combo4039T7; "
            "class C public static method Main(): int "
            "  var r := new Combo4039T7(x := 11); return r.x; "
            "end Main; end C;"),
        11);
}

// ===========================================================================
// T8 — fix #4058 (§9.1/§9.3/§7.9, v0.295): the fast path must fire and
// construct correctly for a combined-form record with a different-arity
// explicit ctor when the body carries a field WITH ITS OWN INITIALIZER
// (contrast T9's genuinely-uninitialized body field, which still declines).
// Originally #4039 declined this shape too (condition (ii) was "record's
// TOTAL storage equals its positional-field count", i.e. ZERO body storage)
// — over-broad, since the preamble field-init loop above (`if
// (!skipPreambleFieldInit) { for (const sema::Symbol* m : fields) ... }`)
// already runs `extra`'s own initializer unconditionally, before this gate,
// regardless of which construction path follows. #4058 widens the gate to
// admit any body member with a real initializer; this now ACCEPTS and runs,
// same runtime value as the pre-#4039 shape with no secondary ctor at all
// (see the #4058 issue's own control repro). Formerly a control pinning a
// compile-time reject (`body_field_with_initializer_still_declines_
// pending_4001_control_spec_7_9`) — updated in the same commit as the fix,
// per CLAUDE.md's "a fix that reverses a prior rejection must update the
// existing tests that pinned the old behaviour."
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           body_field_with_initializer_now_accepted_spec_9_1_9_3_4058) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4039T8(x: int) "
            "  public extra: int := 42; "
            "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
            "end Combo4039T8; "
            "class C public static method Main(): int "
            "  var r := new Combo4039T8(9); return r.x + r.extra; "
            "end Main; end C;"),
        51);
}

// ===========================================================================
// T9 — THE ORIGINAL #4039 REPRO: a combined-form record with a body field of
// CLASS type and NO initializer (so it genuinely has no value the compiler
// can supply via the primary constructor) plus an explicit constructor of a
// different arity that DOES assign it. Before the fix: `Severity::Warning`,
// compile EXIT 0, runtime SIGSEGV. After the fix: a compile-time Error at a
// real source location, never a crash — asserted via
// `failedAtCompileTimeWithRealLocation` (not a bare `EXPECT_FALSE(r.ran)`,
// which cannot distinguish a rejection from a compiler crash, issue #3892).
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           original_repro_uninitialized_body_field_rejected_not_crashed_spec_9_1) {
    auto r = runAot(
        "module M; "
        "class Widget4039 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end Widget4039; "
        "record Combo4039T9(X: int) "
        "  public Extra: Widget4039; "
        "  public constructor(x: int, w: Widget4039) X := x; Extra := w; end constructor; "
        "end Combo4039T9; "
        "class Program4039 "
        "  public static method Main(): int "
        "    var c := new Combo4039T9(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end Program4039;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ===========================================================================
// T10 — the #4017 SEMA boundary this fix does NOT touch, pinned as a KNOWN,
// still-open control (not asserted as fixed): calling the record's OWN
// explicit constructor directly, at that constructor's own (different)
// arity, is currently rejected by sema before codegen ever runs — a
// DIFFERENT defect (#4017) from #4039's codegen mis-compile, sharing the
// same underlying "records never consider their own explicit constructors
// as `new` overload candidates" root in `checkNewExprArgs`. This asserts the
// CURRENT (buggy) sema behaviour so a future #4017 fix updates this test
// deliberately rather than an unrelated change silently flipping it.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           calling_explicit_ctor_at_its_own_arity_still_sema_rejected_4017_control) {
    auto r = runAot(
        "module M; record Combo4039T10(x: int) "
        "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
        "end Combo4039T10; "
        "class C public static method Main(): int "
        "  var r := new Combo4039T10(4, 5); return r.x; "
        "end Main; end C;");
    // UPDATED by fix #4712 (§22.5, §9.1, v0.307) — and this is the update
    // the row was WRITTEN to receive. Its own header said so: it asserted
    // the CURRENT (buggy) sema behaviour "so a future #4017 fix updates
    // this test deliberately rather than an unrelated change silently
    // flipping it." #4712 is that fix: a combined-form record's explicit
    // constructors are now `new` overload candidates, so `new
    // Combo4039T10(4, 5)` resolves to `constructor(a, b)` instead of being
    // rejected against the 1-arg implicit positional constructor.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(9, r.exitCode);  // 4 + 5, via the explicit constructor
}

// ===========================================================================
// T11 — CROSS-MODULE axis (issue #4022's caution: an AST-gated fix can be
// local-axis-only). The library module declares the SAME shape as T3 —
// combined-form record, positional field, different-arity explicit ctor, no
// body storage — and the importer constructs it via the positional arity.
// `hasAnyConstructor`/`ctorForArgs`/`sema::recordPositionalFieldSymbols` all
// read `sema::TypeSymbol` state `ModuleLoader` populates identically for an
// imported type (no `ast::TypeDecl`-only reasoning in this fix), so this is
// expected to pass; it is measured here rather than assumed.
// ===========================================================================
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           cross_module_different_arity_explicit_ctor_uses_primary_spec_9_1) {
    const std::string lib =
        "module Lib4039; "
        "public record ComboX4039(x: int) "
        "  public constructor(x: int, y: int) this.x := x + y; end constructor; "
        "end ComboX4039;";
    const std::string app =
        "module App4039; import Lib4039; "
        "class Program4039 public static method Main(): int "
        "  var c := new ComboX4039(9); return c.x; "
        "end Main; end Program4039;";
    BEZEL_EXPECT_EQ(runXmod4039(lib, app), 9);
}

// ===========================================================================
// T12-T17 — issue #4058 breadth grid. #4058 widens condition (ii) of the
// #4039 fast-path gate from "zero body storage" to "every body member either
// is positional, or has a real initializer the preamble loop already runs"
// (`Emitter::buildRecordValue`'s `bodyStorageAdmitsFastPath`,
// LlvmBackend_Exprs2.cpp). These pin the shapes adjacent to T8 that exercise
// that predicate's actual code paths: the PropertyDecl arm (not just
// FieldDecl), walking MULTIPLE body members (not just one), and the
// still-conservative IMPORTED axis — plus a generic instantiation, since the
// predicate reads `m->decl` (AST-level), not the substituted type.
// ===========================================================================

// T12 — the PropertyDecl/`autoInitializer` arm of `bodyStorageAdmitsFastPath`
// (T8 exercises the FieldDecl/`initializer` arm only): a body AUTO-PROPERTY
// with its own initializer must be admitted and construct correctly, exactly
// like a plain field.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           body_autoproperty_with_initializer_accepted_spec_9_2_4058) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4058T12(x: int) "
            "  public property extra: int := 42; "
            "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
            "end Combo4058T12; "
            "class C public static method Main(): int "
            "  var r := new Combo4058T12(9); return r.x + r.extra; "
            "end Main; end C;"),
        51);
}

// T13 — a body AUTO-PROPERTY with NO initializer (unlike T12), assigned only
// inside the different-arity secondary constructor, remains correctly
// declined: §7.9's definite-assignment concern (#4001, not fixed by #4058)
// is real for this shape — the primary ctor's fast path has no way to run
// that assignment, so admitting it would leave `extra` at its zero default
// regardless of what the (unreachable-from-here) secondary ctor says.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           body_autoproperty_no_initializer_still_declines_spec_7_9_4058) {
    auto r = runAot(
        "module M; record Combo4058T13(x: int) "
        "  public property extra: int; "
        "  public constructor(a: int, b: int) this.x := a + b; this.extra := a; "
        "  end constructor; "
        "end Combo4058T13; "
        "class C public static method Main(): int "
        "  var r := new Combo4058T13(9); return r.x + r.extra; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// T14 — MULTIPLE body fields, ALL with initializers: `bodyStorageAdmitsFastPath`
// must walk every body-storage member, not just the first, and admit only
// when every one qualifies.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           multiple_body_fields_all_initialized_accepted_spec_9_3_4058) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4058T14(x: int) "
            "  public a: int := 10; "
            "  public b: int := 20; "
            "  public constructor(p: int, q: int) this.x := p + q; end constructor; "
            "end Combo4058T14; "
            "class C public static method Main(): int "
            "  var r := new Combo4058T14(9); return r.x + r.a + r.b; "
            "end Main; end C;"),
        39);
}

// T15 — MIXED: one body field initialized, a SECOND with no initializer.
// `bodyStorageAdmitsFastPath` requires EVERY body member to qualify — a
// single uninitialized member must still decline the whole call, not just
// leave that one field unwritten.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           multiple_body_fields_one_uninitialized_still_declines_spec_7_9_4058) {
    auto r = runAot(
        "module M; record Combo4058T15(x: int) "
        "  public a: int := 10; "
        "  public b: int; "
        "  public constructor(p: int, q: int) this.x := p + q; this.b := p; "
        "  end constructor; "
        "end Combo4058T15; "
        "class C public static method Main(): int "
        "  var r := new Combo4058T15(9); return r.x + r.a + r.b; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// T16 — GENERIC axis: the same T8 shape, but the record is generic and the
// call site is a concrete instantiation. `bodyStorageAdmitsFastPath` reads
// `m->decl` (the AST `FieldDecl`/`PropertyDecl`), not the substituted type,
// so genericity is not expected to matter — measured rather than assumed
// (mirrors #4039's own local/cross-module double-check discipline).
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           generic_body_field_with_initializer_accepted_spec_9_1_4058) {
    BEZEL_EXPECT_EQ(
        run("module M; record Combo4058T16<T>(x: T) "
            "  public extra: int := 42; "
            "  public constructor(a: T, b: T) this.x := a; end constructor; "
            "end Combo4058T16; "
            "class C public static method Main(): int "
            "  var r := new Combo4058T16<int>(9); return r.x + r.extra; "
            "end Main; end C;"),
        51);
}

// T17 — CROSS-MODULE flip (fix #4103, §9.1/§9.3/§7.9, v0.295): the T8 shape
// is now CORRECT across the module boundary. `bodyStorageAdmitsFastPath`
// (LlvmBackend_Exprs2.cpp) used to decline every imported body member with
// `m->decl == nullptr` because no `ast::FieldDecl`/`PropertyDecl` crosses
// the module boundary — the #3671-documented seam — but the preamble loop
// that runs UNCONDITIONALLY before this gate ALREADY services an imported
// body member via `importedInstanceFieldInitExpr` (#2874/#3723 parsed-AST-
// expr replay) and `importedConstValue`/`foldImportedConstText`
// (#2467/#328/#3723 const-fold replay). The predicate now mirrors those
// success paths instead of a coarser `m->decl`-alone answer, so a T8-shape
// IMPORTED call must construct and run correctly — the runtime value
// `9 + 42 = 51` proves the imported `extra := 42` was replayed into the
// slot. Pre-#4103 this cell was a -999 decline (see the git history of
// the T17 comment block), over-rejecting exactly what the preamble loop
// could have answered; the fix flips it to the success path the cross-
// module run already proved in the issue's control shape.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           cross_module_body_field_with_initializer_accepted_spec_4103) {
    const std::string lib =
        "module Lib4103; "
        "public record ComboXM4103(x: int) "
        "  public extra: int := 42; "
        "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
        "end ComboXM4103;";
    const std::string app =
        "module App4103; import Lib4103; "
        "class Program4103 public static method Main(): int "
        "  var r := new ComboXM4103(9); return r.x + r.extra; "
        "end Main; end Program4103;";
    BEZEL_EXPECT_EQ(runXmod4039(lib, app), 51);
}

// T17b — CROSS-MODULE body AUTO-PROPERTY with initializer (the
// PropertyDecl/`autoInitializer` arm of the imported-axis fix, mirroring
// T12's LOCAL twin). The preamble loop's parsed-expr path fires here
// because `property extra: int := 42;` lowers to the same wire shape a
// body field does; the `importedConstValue` fallback would also fire.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           cross_module_body_autoproperty_with_initializer_accepted_spec_4103) {
    const std::string lib =
        "module Lib4103b; "
        "public record ComboXM4103b(x: int) "
        "  public property extra: int := 42; "
        "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
        "end ComboXM4103b;";
    const std::string app =
        "module App4103b; import Lib4103b; "
        "class Program4103b public static method Main(): int "
        "  var r := new ComboXM4103b(9); return r.x + r.extra; "
        "end Main; end Program4103b;";
    BEZEL_EXPECT_EQ(runXmod4039(lib, app), 51);
}

// T17c — CROSS-MODULE, MIXED: one body field WITH initializer + one body
// field WITHOUT. Mirrors T15's LOCAL twin — the predicate must walk every
// body-storage member, not just the first, and decline the fast path when
// any one lacks a replayable initializer (the latter would leave its slot
// at the `ConstantAggregateZero` default and silently read as zero — the
// hazard this whole predicate exists to prevent). Soundness: the call
// must still be a LOCATED compile-time Error, never a silent wrong value
// or crash — the same hazard #4039 fixed in the first place and that
// #4058's T15 already pinned on the LOCAL axis.
BEZEL_TEST(codegen_combined_record_secondary_ctor_arity_4039,
           cross_module_body_field_mixed_initializers_still_declines_spec_4103) {
    const std::string lib =
        "module Lib4103c; "
        "public record ComboXM4103c(x: int) "
        "  public extra: int := 42; "
        "  public uninit: int; "
        "  public constructor(a: int, b: int) this.x := a + b; end constructor; "
        "end ComboXM4103c;";
    const std::string app =
        "module App4103c; import Lib4103c; "
        "class Program4103c public static method Main(): int "
        "  var r := new ComboXM4103c(9); return r.x + r.extra + r.uninit; "
        "end Main; end Program4103c;";
    // -999 == compile failure; the LOCAL twin (T15) pins "located not crash"
    // via `failedAtCompileTimeWithRealLocation`. The `#4103 fix widens what
    // the predicate ACCEPTS — a member for which neither replay path is
    // available (no parsed-expr, no const-fold text) still falls through to
    // `return false` and the terminal Error arm, so this shape is soundly
    // declined cross-module too.
    BEZEL_EXPECT_EQ(runXmod4039(lib, app), -999);
}
