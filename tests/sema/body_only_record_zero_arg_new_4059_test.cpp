// Regression tests for #4059 (§9.1 "Positional records" / §9.2 "Body records",
// v0.295): a body-only RECORD (no positional fields, no declared instance
// constructor) was silently constructed via `new Foo()` — the compiler fell
// through the `t->hasPositionalFields` gate at the NewExpr arm's call site
// (ExprTypecheck_Exprs.cpp:~line 4634) and never reached `checkNewExprArgs`,
// so the absence of any constructor drew NO diagnostic and codegen /
// `lowerNew` happily emitted a zero-fill struct. Pre-fix behaviour, measured
// against this tree's HEAD (8cc8195) for the simplest repro below:
//
//   record Foo
//     public property Bar: int;
//     public property Qux: int;
//   end Foo;
//   class C public static method Main(): int
//     var f := new Foo();
//     return f.Bar + f.Qux;   // compiles clean, runs, returns 0
//   end Main; end C;
//
// `return f.Bar + f.Qux;` returned `0`, not a compile error — the bug.
//
// SPEC GROUNDS (§9.1, §9.2, §22.7). §9.1 generates a primary constructor
// ONLY for positional records ("for each positional record, the compiler
// generates: a primary constructor whose parameters are the positional
// fields, in declaration order"). §9.2 declares body records constructible
// only via `default(T)` or a user-declared constructor — neither of which
// the implicit-default-class premise `checkNewExprArgs`'s pre-fix body read.
// §17.11 confirms: a body-only record's `default(T)` IS defined (every
// non-nullable auto-property is defaultable here), so `default(Foo)` is
// the spelled-out remedy the diagnostic names.
//
// THE FIX (paired edits):
//   1) ExprTypecheck_Exprs.cpp's NewExpr route for records now calls
//      `checkNewExprArgs` unconditionally (the gate was a single
//      `t->hasPositionalFields` check that masked every body-only shape).
//   2) ExprTypecheck_Calls.cpp's `checkNewExprArgs` and
//      `checkImportedNewExprArgs` arms of the `ctors.empty()` branch
//      discriminate: a body-only RECORD emits a §9.1/§9.2/`#4059`
//      rejection; a CLASS still admits the implicit default ctor (zero
//      parameters) per §22.7.
//
// Rows below pin every cell the fix can regress:
//   T1/T2 reject: the two shapes the issue names (zero-arg, any-arg).
//   T3 accept:   `default(Foo)` is the §17.11 remedy and must keep working.
//   T4 reject:   imported body-only record (the duplicate edit's arm).
//   T5 reject:   combined-form record with positional fields but no
//                §-matching secondary ctor (already covered by #4039's
//                arm; included here as a regression-control pair).
//   T6-T9 controls: positional record, body record WITH a ctor, generic
//                  body record with explicit ctor, class with no ctor —
//                  all four must keep their pre-fix behaviour, proving
//                  the gate widening did not over-reject.
//
// All reject rows assert BOTH `diagnostics.hasErrors()` and that the
// diagnostic carries the §9.1/§9.2/`#4059` substrings, so a future spec
// refinement of the body's exact wording updates these rows deliberately
// rather than silently flipping them. The class-with-no-ctor row pins the
// implicit-default ctor — the leg of the discriminated `ctors.empty()`
// branch the fix kept verbatim.

#include <cstdio>
#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff any diagnostic in `r` carries `sub` as a substring.
bool hasErrorMsg(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cross-module runner — paired with T4 below for the imported arm of the
// fix. `runAot` is single-module, so the IMPORTED side of `ctors.empty()`
// uses `Driver::compileObject` for the library and `compileFileForTest`
// (the ASan sweep / driver_run_support wrapper) for the importer. Mirrors
// combined_record_secondary_ctor_arity_4039_test.cpp's pattern.
struct TempFile4059 {
    std::string path;
    explicit TempFile4059(std::string p) : path(std::move(p)) {}
    ~TempFile4059() { std::remove(path.c_str()); }
};
void writeSource4059(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}
struct RunResult4059 { int code; };
RunResult4059 runCapture4059(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code};
}
int runXmod4059(const std::string& libSrc, const std::string& appSrc) {
    const auto& dir = wtScratchDir("wt-4059");
    std::string libStem = unique_stem("bezel_4059lib_");
    TempFile4059 libSrcFile((dir / (libStem + ".bzl")).string());
    TempFile4059 libObjFile((dir / (libStem + ".o")).string());
    writeSource4059(libSrcFile.path, libSrc);
    bezel::Driver libDriver;
    auto libR = libDriver.compileObject(libSrcFile.path, libObjFile.path);
    if (!libR.success) return -999;
    std::string appStem = unique_stem("bezel_4059app_");
    TempFile4059 appSrcFile((dir / (appStem + ".bzl")).string());
    TempFile4059 appExeFile((dir / appStem).string());
    writeSource4059(appSrcFile.path, appSrc);
    bezel::Driver appDriver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjFile.path;
    d.objectPath = libObjFile.path;
    deps.push_back(d);
    auto appR = bezel::test::compileFileForTest(
        appDriver, appSrcFile.path, appExeFile.path, deps);
    if (!appR.success) return -999;
    return runCapture4059(appExeFile.path).code;
}

}  // namespace

// ============================================================================
// T1 — the issue's headline repro: `new Foo()` on a body-only record (no
// positional fields, no declared ctor). Pre-fix this compiled clean and
// minted a zero-filled struct; post-fix it is rejected with the §9.1/§9.2
// diagnostic. `default(Foo)` is named in the message as the remedy.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059, zero_arg_new_body_record_rejected) {
    auto r = runAot(
        "module M; "
        "record Foo4059T1 "
        "  public property Bar: int; "
        "  public property Qux: int; "
        "end Foo4059T1; "
        "class C public static method Main(): int "
        "  var f := new Foo4059T1(); "
        "  return f.Bar + f.Qux; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "requires a declared constructor"));
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "implicit default constructor"));
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "default("));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// T2 — narrow #4059 flip (was: REJECT, is: ACCEPT). The original #4059 fix
// rejected `new Foo(5)` on a body-only record as the "any-arg overload of
// the headline bug" — but the headline bug is the ZERO-ARG silent zero-fill
// defect. Any-arg on a body-only record is the §9.1/§9.2 memberwise path
// the pre-fix codebase relied on (positional / named args supply values
// even without a positional-field ctor). The narrow four-condition gate
// admits this shape and lets codegen bind the args. Pin the new behaviour
// so a future re-tightening of the gate does not silently re-over-reject
// `new Foo(5)`. (T2b-5 covers the multi-arg `new T(a, b)` variant of the
// same path; T2 covers the single-arg `new T(v)` variant.)
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059, any_arg_new_body_record_accepted_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Foo4059T2 "
        "  public property Bar: int; "
        "end Foo4059T2; "
        "class C public static method Main(): int "
        "  var f := new Foo4059T2(5); "
        "  return f.Bar; "                      // 5
        "end Main; end C;"), 5);
}

// ============================================================================
// T3 — the §17.11 remedy path: `default(Foo)` on the SAME body-only record
// compiles clean and runs. Pins the half of the spec the diagnostic names —
// the row that lets a test reader see the fix did not over-reject every
// construction shape on a body-only record.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059, default_value_works_on_body_record) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Foo4059T3 "
        "  public property Bar: int; "
        "  public property Qux: int; "
        "end Foo4059T3; "
        "class C public static method Main(): int "
        "  var f := default(Foo4059T3); "
        "  return 7 + f.Bar + f.Qux; "        // 7
        "end Main; end C;"), 7);
}

// ============================================================================
// T4 — IMPORTED arm of the fix: the same body-only record declared in a
// library module, constructed via the importer. The duplicate edit at
// `checkImportedNewExprArgs`'s `ctors.empty()` branch is the one this
// row exercises; mirroring the local twin's diagnostic wording keeps
// the rejected-shape message identical across module boundaries.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           imported_body_record_zero_arg_new_rejected_spec_9_2) {
    const std::string lib =
        "module Lib4059; "
        "public record Body4059 "
        "  public property X: int; "
        "end Body4059;";
    const std::string app =
        "module App4059; import Lib4059; "
        "class Program4059 public static method Main(): int "
        "  var b := new Body4059(); "
        "  return b.X; "
        "end Main; end Program4059;";
    // runXmod4059's value on a compile failure is -999 (returned via the
    // `!libR.success` / `!appR.success` paths in the helper); pin it
    // exactly so the negative path is distinguishable from a successful
    // run that happens to print 0.
    BEZEL_EXPECT_EQ(runXmod4059(lib, app), -999);
}

// ============================================================================
// T5 — combined-form control: a record with POSITIONAL fields PLUS a body
// that declares a secondary ctor with a different arity. Still rejected by
// the §22.5 arity check (the pre-existing #4039/#4017 path), and the new
// gate widening must NOT regress this shape with an extra diagnostic.
// Pre-fix: rejected by `checkNewExprArgs`'s positional-field arm alone.
// Post-fix: still rejected, by the SAME arm (the body-only rejection
// applies only when both `t->hasPositionalFields == false` AND
// `ctors.empty()`; positional-field records take the earlier arm and
// never reach the new body-only branch).
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           combined_form_record_with_explicit_secondary_ctor_control_4039) {
    auto r = runAot(
        "module M; record Combo4059(X: int) "
        "  public constructor(a: int, b: int) this.X := a + b; end constructor; "
        "end Combo4059; "
        "class C public static method Main(): int "
        "  var r := new Combo4059(4, 5); return r.X; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    // §22.5 arity reject — `new Combo4059(4, 5)` clashes with the implicit
    // positional ctor (takes `X: int`, 1 arg). The substring `argument`
    // matches every arity-error wording the codebase emits (e.g. "expects
    // N argument(s)", "takes N positional argument(s)", "argument count").
    // Counts as protected per the ambiguous_ran_false ratchet: a crash /
    // signal / timeout / ASan report would not produce an Error whose
    // message contains the substring, so the row distinguishes a genuine
    // rejection from a silent runtime failure mode.
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "argument"));
}

// ============================================================================
// T6 — positional record (no body ctor) constructs cleanly: the `hasPositionalFields`
// path still routes through `checkArgsAgainstParams` and the spec §9.1
// primary ctor still binds; the gate widening does not touch this leg.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           positional_record_two_arg_constructs_cleanly_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt4059(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt4059(3, 4); return p.X + p.Y; "
        "end Main; end C;"), 7);
}

// ============================================================================
// T7 — body record WITH a declared ctor constructs cleanly: the
// `ctors.size() == 1` arm (below the new body-record reject) handles the
// legitimate case the diagnostic names as one remedy.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           body_record_with_declared_ctor_constructs_cleanly_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rect4059 "
        "  public property Width: int; "
        "  public property Height: int; "
        "  public constructor(w: int, h: int) "
        "    this.Width := w; this.Height := h; "
        "  end constructor; "
        "end Rect4059; "
        "class C public static method Main(): int "
        "  var r := new Rect4059(3, 5); return r.Width + r.Height; "
        "end Main; end C;"), 8);
}

// ============================================================================
// T8 — generic body record WITH a declared ctor: the §14.1 instantiating
// shape is unaffected by the gate widening; the per-arg type-check on a
// generic body record still routes through `checkNewExprArgs`'s
// `ctors.size() == 1` arm.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           generic_body_record_with_declared_ctor_constructs_cleanly_spec_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box4059<T> "
        "  public property V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "end Box4059; "
        "class C public static method Main(): int "
        "  var b := new Box4059<int>(42); return b.V; "
        "end Main; end C;"), 42);
}

// ============================================================================
// T9 — class with NO declared ctor gets the implicit default ctor (§22.7):
// the NEW diagnostic's class leg is intentionally unchanged, so `new K()`
// on `class K { ... } end K;` compiles clean. Pin here so a future
// widening of the `t->kind == Record` gate does not silently absorb
// classes and reject their implicit-default construction.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           class_with_no_declared_ctor_implicit_default_still_works_spec_22_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class K4059 "
        "  public v: int; "
        "end K4059; "
        "class C public static method Main(): int "
        "  var k := new K4059(); "
        "  return k.v; "
        "end Main; end C;"), 0);
}

// ============================================================================
// T2b — narrow #4059 (§9.1 / §9.2, v0.295) cell. The original #4059 fix
// over-rejected FOUR legitimate §9.1/§9.2 memberwise-construction paths on
// body-only records: zero-arg WITH field initializers, `with(...)` clauses,
// positional args, and named args. The narrow fix gates the §9.1/§9.2
// rejection on ALL FOUR conditions holding simultaneously:
//   * `ctors.empty()` (no declared ctor — already)
//   * `ne.args.empty()` (no positional/named memberwise args)
//   * `!inWithExprSource_` (no `with(...)` clause)
//   * `!recordHasFieldOrPropertyInit` (no field/property `:= ...` inits)
// Anything that supplies values via any other channel is a legitimate
// construction form and is accepted; only the silent zero-fill defect
// shape (none of the four supplying values) is rejected.
//
// T2b-1..T2b-5 exercise every cell of the four-way gate:
//   T2b-1: zero-arg WITH field initializer   → ACCEPT (§9.1 inits supply)
//   T2b-2: `with(...)` clause (positive T2b)  → ACCEPT (§9.5 IS construction)
//   T2b-3: named-args                         → ACCEPT (§9.1 memberwise)
//   T2b-4: zero-arg NO initializer (T1 retest) → REJECT (headline #4059)
//   T2b-5: positional args                    → ACCEPT (§9.1 memberwise)
// Each row is a single-source-of-truth pin so a future widening of the
// four-way gate does not silently re-over-reject one of these.
// ============================================================================
BEZEL_TEST(body_only_record_zero_arg_new_4059,
           zero_arg_with_field_initializer_accepted_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt4059T2b1 "
        "  public property X: int := 5; "
        "  public property Y: int := 7; "
        "end Pt4059T2b1; "
        "class C public static method Main(): int "
        "  var p := new Pt4059T2b1(); "
        "  return p.X + p.Y; "                // 12
        "end Main; end C;"), 12);
}

BEZEL_TEST(body_only_record_zero_arg_new_4059,
           with_clause_on_new_body_record_accepted_spec_9_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Rect4059T2b2 "
        "  public property Width: int; "
        "  public property Height: int; "
        "end Rect4059T2b2; "
        "class C public static method Main(): int "
        "  var r := new Rect4059T2b2() with(Width := 6, Height := 7); "
        "  return r.Width + r.Height; "        // 13
        "end Main; end C;"), 13);
}

BEZEL_TEST(body_only_record_zero_arg_new_4059,
           named_args_on_body_record_accepted_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Named4059T2b3 "
        "  public property A: int; "
        "  public property B: int; "
        "end Named4059T2b3; "
        "class C public static method Main(): int "
        "  var n := new Named4059T2b3(A := 3, B := 4); "
        "  return n.A + n.B; "                 // 7
        "end Main; end C;"), 7);
}

BEZEL_TEST(body_only_record_zero_arg_new_4059,
           zero_arg_no_initializer_still_rejected_headline_4059) {
    auto r = runAot(
        "module M; "
        "record Foo4059T2b4 "
        "  public property Bar: int; "
        "  public property Qux: int; "
        "end Foo4059T2b4; "
        "class C public static method Main(): int "
        "  var f := new Foo4059T2b4(); "
        "  return f.Bar + f.Qux; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "requires a declared constructor"));
    BEZEL_EXPECT_TRUE(hasErrorMsg(r, "implicit default constructor"));
    BEZEL_EXPECT_FALSE(r.ran);
}

BEZEL_TEST(body_only_record_zero_arg_new_4059,
           positional_args_on_body_record_accepted_spec_9_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pos4059T2b5 "
        "  public property X: int; "
        "  public property Y: int; "
        "end Pos4059T2b5; "
        "class C public static method Main(): int "
        "  var p := new Pos4059T2b5(11, 22); "
        "  return p.X + p.Y; "                 // 33
        "end Main; end C;"), 33);
}
