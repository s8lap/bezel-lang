// Codegen: §22.7 "Observing `this` during construction" — the recorded
// runtime residual (issue #1748, spec v0.306).
//
// §22.7's three static rules (rule 1 the observation gate, rule 2 no virtual
// dispatch from non-sealed constructor bodies, rule 3 closures observe at
// invocation, not at capture) reject every UNSAFE `this`-observation they can
// SEE at compile time via checkDefiniteAssignment — but the section says so
// itself: "These checks reject every unsafe observation they can see and are
// **not a completeness guarantee**": a virtual call reached through a
// non-virtual helper called after the gate, external code reached by an
// admitted `this`-escape that dispatches back, or a during-construction
// closure invocation (rule 3's own carve-out — capturing `this` is NOT gated
// by rule 1, so an admitted closure invoked before construction completes is
// a construction-time observation the static rules never saw) can still read
// a field before its constructor-body assignment runs.
//
// The section's recorded runtime bound (the "residual"): "before construction
// begins, every reference-typed field of the allocation is initialized to
// `null` ... a read of a non-nullable reference field still at that null
// watermark raises NullReferenceException (§31.4) in checked disposal mode
// (§28.2) and, unchecked, observes the watermark `null` with undefined
// behavior on any use requiring non-null — the same checked/unchecked split
// as use-after-dispose."
//
// PRE-FIX: checked-disposal mode had NO watermark guard at all — a program
// exercising the residual segfaulted IDENTICALLY in both --checked-disposal
// and default (unchecked) builds, instead of raising NullReferenceException
// in the checked build. Confirmed by a manual bisect against origin/main
// HEAD (7f29b8ce): the row 1 program below segfaults (SIGSEGV, exit 139)
// under BOTH runAot and runAotCheckedDisposal pre-fix.
//
// THE FIX: LlvmBackend_Layout.cpp's new `Emitter::emitFieldWatermarkGuard`,
// called from the two instance-field READ sites (CG8 `fieldAddr`-qualified
// reads in LlvmBackend_Exprs.cpp, and CG9 `implicitFieldAddr`-bare reads,
// same file) — see that function's own comment for the full rationale for
// why a checkedDisposal_-gated, unconditional-on-every-read guard is both
// necessary (the three routes above have no static link back to "is this
// mid-construction") and sound (post-construction, §7.9's definite-assignment
// guarantee means a non-nullable reference field can never legitimately be
// null, so the guard is a no-op everywhere except a genuine violation).
//
// Row map:
//   1. REGRESSION — the issue's own repro shape: a `this`-capturing closure,
//      admitted by rule 3, invoked synchronously from the constructor body
//      BEFORE the non-nullable reference field it reads is assigned. Checked
//      mode must raise NullReferenceException (caught, distinct return).
//   2. CONTROL (unchecked) — the identical program, default (unchecked)
//      build: §22.7 permits undefined behavior here (the pre-fix SIGSEGV);
//      the fix does not touch unchecked codegen at all, so this must still
//      never reach the "caught NullReferenceException" outcome.
//   3. CONTROL (nullable field) — the SAME shape but the field is `string?`:
//      per §22.7 only a NON-NULLABLE reference field's watermark read raises;
//      a nullable field legitimately observes `null`. Pins that the fix does
//      not "silently over-apply" to nullable fields.
//   4. CONTROL (defaultable value field) — an `int` field read through the
//      identical closure-during-construction shape: a value-typed field has
//      a well-defined §7.9 default (`0`) and must never raise, in either
//      mode. Pins that the fix is scoped to reference types only.
//   5. CONTROL (well-formed program) — the field is assigned BEFORE the
//      closure is invoked (the ordinary, spec-legal ordering). Checked and
//      unchecked builds must produce IDENTICAL output — the guard must be a
//      true no-op on every correct program.
//   6. CONTROL (generic bare-T at a nullable instantiation) — an adversarial-
//      review finding on rows 1-5's own fix, not the original issue repro:
//      `Box<T>`'s field `_v: T` is a bare, unconstrained type parameter, and
//      `sema::isNonNullableReferenceType` reads T's UNSUBSTITUTED declared
//      TypeRef — always Named/no-declaration for a bare T, §7.10's own
//      conservative default for DEFINITE ASSIGNMENT, not a claim that every
//      read is non-null (§7.10: "reads remain conservatively possibly-null
//      per this section"). Without resolving through the active
//      monomorphization binding, `new Box<string?>(null); var s := b.Get();`
//      was misread as a non-nullable-field read and raised a SPURIOUS
//      NullReferenceException in checked mode on a CORRECT program. Checked
//      and unchecked must both observe `null` and return 7.
//
// Mechanically verified FAIL-WITHOUT, in two stages:
//   Stage 1 (rows 1-5, the original fix): saved the diff (LlvmBackend_Exprs.cpp,
//   LlvmBackend_Layout.cpp, LlvmBackend_impl.hpp) to a patch, reverted with
//   `git apply -R` (NOT git stash — a worktree-shared ref), rebuilt, and
//   re-ran this filter: row 1 FAILED (checked build segfaulted, exit 139,
//   never reached the catch's return 1) while rows 2-5 stayed passing (none
//   of them depend on the fix). Re-applied, rebuilt, all five passed.
//   Stage 2 (row 6, the adversarial-review follow-up): with rows 1-5's fix
//   IN PLACE, temporarily removed just the `resolveTypeParamRef(fty)` call
//   in `emitFieldWatermarkGuard` (LlvmBackend_Layout.cpp), leaving `fty :=
//   ctx_.exprType(e)` unresolved; rebuilt: row 6's checked-mode assertion
//   FAILED (the guard fired on the still-null `string?` field before the
//   generic substitution was consulted, raising NullReferenceException
//   instead of reaching `return 7`), row 6's unchecked-mode assertion still
//   passed (unaffected by checkedDisposal_). Restored `resolveTypeParamRef`,
//   rebuilt, both row 6 assertions passed. `BEZEL_DUMP_IR=1` on this exact
//   program under --checked-disposal confirms `Box$Get$spec` lowers to bare
//   `getelementptr`/`load`/`ret` — no compare — identical to the unchecked
//   build's shape, i.e. the guard is provably ABSENT there, not merely
//   not-taken at runtime.
//
// Spec: §22.7 "Observing `this` during construction" (v0.306), §28.2
// "Use after disposal; checked disposal mode", §31.4 "Standard exception
// types" (NullReferenceException), §7.9 "Definite assignment for non-
// nullable fields", §7.10 "Generics and nullability" (row 6).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// Convenience wrappers mirroring checked_disposal_601_test.cpp's shape.
int runChecked(const std::string& program) {
    JitResult r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}
int runDefault(const std::string& program) {
    JitResult r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Row 1/2 program: a `this`-capturing closure (rule 3, admitted — capture is
// not gated by rule 1) invoked synchronously from the constructor body BEFORE
// `_name` (a non-nullable `string` field) is assigned. The closure's bare
// `_name` read is CG9 (implicit-`this`); `.Length` is evaluated only if the
// read does not throw, so a raised NullReferenceException is observed at the
// field read itself, exactly as §22.7's residual states.
const char* kNonNullableFieldProgram =
    "module M; "
    "class W "
    "  private _name: string; "
    "  public constructor() "
    "    var f := lambda() => Console.WriteLine(_name.Length); "
    "    f(); "
    "    _name := \"hi\"; "
    "  end constructor; "
    "end W; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      var w := new W(); "
    "      w.Dispose(); "
    "      return 0; "
    "    catch NullReferenceException => return 1; "
    "    end; "
    "  end Main; "
    "end C;";

// Row 3 program: identical shape, but `_name` is a NULLABLE `string?` field.
// §22.7's residual is scoped to NON-nullable reference fields only — a
// nullable field's watermark read legitimately observes `null` (no default
// per §7.9's own text: "every nullable type carries a default (null) and is
// not reported"). The lambda reads `_name is null` (true, since it is called
// before the field is assigned) and returns 2; the field is then assigned and
// Main returns 0 without ever entering the catch arm.
const char* kNullableFieldProgram =
    "module M; "
    "class W "
    "  private _name: string?; "
    "  public constructor() "
    "    var f := lambda() => Console.WriteLine(_name is null); "
    "    f(); "
    "    _name := \"hi\"; "
    "  end constructor; "
    "end W; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      var w := new W(); "
    "      w.Dispose(); "
    "      return 0; "
    "    catch NullReferenceException => return 1; "
    "    end; "
    "  end Main; "
    "end C;";

// Row 4 program: identical shape, but `_count` is a defaultable VALUE-typed
// `int` field. §22.7's watermark text is scoped to "reference-typed" fields;
// a value-typed field's §7.9 default (`0`) is always well-defined and must
// never raise, in either mode.
const char* kValueFieldProgram =
    "module M; "
    "class W "
    "  private _count: int; "
    "  public constructor() "
    "    var f := lambda() => Console.WriteLine(_count); "
    "    f(); "
    "    _count := 5; "
    "  end constructor; "
    "end W; "
    "class C "
    "  public static method Main(): int "
    "    var w := new W(); "
    "    w.Dispose(); "
    "    return 0; "
    "  end Main; "
    "end C;";

// Row 5 program: the SAME `_name: string` field and closure shape, but the
// field is assigned BEFORE the closure is invoked (the ordinary, correct
// ordering). Both modes must produce the field's real length (2, for "hi")
// with no exception raised — the guard must be a true no-op on a correct
// program.
const char* kWellFormedProgram =
    "module M; "
    "class W "
    "  private _name: string; "
    "  public constructor() "
    "    _name := \"hi\"; "
    "    var f := lambda() => Console.WriteLine(_name.Length); "
    "    f(); "
    "  end constructor; "
    "  public method Get(): string return _name; end Get; "
    "end W; "
    "class C "
    "  public static method Main(): int "
    "    var w := new W(); "
    "    var n := w.Get().Length; "
    "    w.Dispose(); "
    "    return n; "
    "  end Main; "
    "end C;";

// Row 6 program: an ADVERSARIAL-REVIEW finding on the fix above, not the
// original issue repro. `Box<T>`'s own field `_v: T` is a bare, UNCONSTRAINED
// type parameter — per §7.10 "Generics and nullability" (v0.306), "a field
// ... of bare type T ... follows §7.9's non-nullable-reference rules ...
// since T may be instantiated non-nullable; *reads* remain conservatively
// possibly-null per this section." `sema::isNonNullableReferenceType` reads
// the UNSUBSTITUTED declared TypeRef, which for a bare-T field is always
// Named/no-declaration — §7.10's own conservative default for the DEFINITE-
// ASSIGNMENT obligation, not a claim that every read is non-null. Without
// resolving through the active monomorphization binding first, the row 1
// fix's guard misread `new Box<string?>(null); var s := b.Get();` as reading
// a non-nullable field, and raised a SPURIOUS NullReferenceException in
// checked-disposal mode on a CORRECT program (`string?` legitimately holds
// `null`) — turning working code into an unhandled exception. Both modes
// must observe the same `null` and take the same branch.
const char* kGenericBareTypeParamNullableInstantiationProgram =
    "module M; "
    "class Box<T> "
    "  private _v: T; "
    "  public constructor(v: T) _v := v; end constructor; "
    "  public method Get(): T return _v; end Get; "
    "end Box; "
    "class Program "
    "  public static method Main(): int "
    "    var b := new Box<string?>(null); "
    "    var s := b.Get(); "
    "    if s = null then "
    "      return 7; "
    "    end; "
    "    return 8; "
    "  end Main; "
    "end Program;";

}  // namespace

// ── Row 1: REGRESSION — checked mode raises NullReferenceException ───────

BEZEL_TEST(ctor_this_observation_residual_1748,
           closure_invoked_during_construction_checked_raises_nre) {
    BEZEL_EXPECT_EQ(runChecked(kNonNullableFieldProgram), 1);
}

// ── Row 2: CONTROL — unchecked mode never reaches the checked-mode outcome ─

BEZEL_TEST(ctor_this_observation_residual_1748,
           closure_invoked_during_construction_unchecked_is_ub_not_nre) {
    // §22.7: unchecked observes the watermark `null` with undefined behavior
    // on any use requiring non-null — the pre-fix (and still-unchecked)
    // outcome is a SIGSEGV (r.ran == false, runAot's crash convention), never
    // the checked build's caught-NullReferenceException return value 1. The
    // fix touches ONLY the checkedDisposal_-gated guard, so this must be
    // unchanged by it.
    BEZEL_EXPECT_TRUE(runDefault(kNonNullableFieldProgram) != 1);
}

// ── Row 3: CONTROL — a nullable field's watermark read never raises ──────

BEZEL_TEST(ctor_this_observation_residual_1748,
           nullable_field_watermark_read_does_not_raise_checked) {
    BEZEL_EXPECT_EQ(runChecked(kNullableFieldProgram), 0);
}

BEZEL_TEST(ctor_this_observation_residual_1748,
           nullable_field_watermark_read_does_not_raise_unchecked) {
    BEZEL_EXPECT_EQ(runDefault(kNullableFieldProgram), 0);
}

// ── Row 4: CONTROL — a defaultable value-typed field never raises ────────

BEZEL_TEST(ctor_this_observation_residual_1748,
           value_typed_field_watermark_read_does_not_raise_checked) {
    BEZEL_EXPECT_EQ(runChecked(kValueFieldProgram), 0);
}

BEZEL_TEST(ctor_this_observation_residual_1748,
           value_typed_field_watermark_read_does_not_raise_unchecked) {
    BEZEL_EXPECT_EQ(runDefault(kValueFieldProgram), 0);
}

// ── Row 5: CONTROL — a well-formed program is unaffected in both modes ───

BEZEL_TEST(ctor_this_observation_residual_1748,
           well_formed_program_identical_in_both_modes) {
    BEZEL_EXPECT_EQ(runChecked(kWellFormedProgram), 2);
    BEZEL_EXPECT_EQ(runDefault(kWellFormedProgram), 2);
}

// ── Row 6: CONTROL — a generic bare-T field at a nullable instantiation ──
// (adversarial-review finding on this fix; §7.10, not §22.7 directly — the
// guard's own classification must not over-apply to a resolvable bare T.)

BEZEL_TEST(ctor_this_observation_residual_1748,
           generic_bare_type_param_nullable_instantiation_checked) {
    BEZEL_EXPECT_EQ(
        runChecked(kGenericBareTypeParamNullableInstantiationProgram), 7);
}

BEZEL_TEST(ctor_this_observation_residual_1748,
           generic_bare_type_param_nullable_instantiation_unchecked) {
    BEZEL_EXPECT_EQ(
        runDefault(kGenericBareTypeParamNullableInstantiationProgram), 7);
}
