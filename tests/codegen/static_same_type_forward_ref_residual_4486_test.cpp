// fix #4486 / §22.13 (v0.306): the same-type forward-reference RUNTIME
// residual — distinct from, but built on the same bound as, §22.7's residual
// ("§22.7's residual bound, applied to static storage").
//
// §22.13's forward-reference paragraph (~line 5463): "if the initializer
// expression for a static readonly field F reads a static readonly field G
// of the same type declared after F in the source ... the reference is
// rejected at compile time. ... Like the cross-type graph, the transitive
// check is a deliberate approximation, not a completeness guarantee: a
// same-type forward read routed through a function value, virtual dispatch,
// or a module boundary compiles, and at runtime observes the field's §7.9
// default — for a non-nullable reference static, the null watermark, which
// raises NullReferenceException at the read in checked disposal mode
// (§22.7's residual bound, applied to static storage) and is undefined
// behavior to use unchecked."
//
// PRE-FIX: `Emitter::emitStaticPartialReadGuard` (LlvmBackend_Setup.cpp)
// could not distinguish this same-type case from genuine CROSS-type
// re-entrancy (the T -> U -> T signature, §22.13 rule 2) — both landed on the
// same same-thread/not-yet-assigned test and both raised the synthesized
// TypeInitializationException. Confirmed by direct mechanism trace and by
// FAIL-WITHOUT below.
//
// THE FIX (this commit): a per-thread MONOTONIC epoch, `bz_static_init_epoch`
// (bezel_runtime_concurrency.cpp), bumped on every type's CAS-win (fresh
// 0->1 transition to in-progress) on this thread. Each type snapshots the
// POST-bump epoch at its own CAS-win (`bz$T$initepoch`, emitEnsureInitFns).
// `emitStaticPartialReadGuard` compares the CURRENT epoch against the
// field's owning type's snapshot: EQUAL => no other type's init has
// started on this thread since the owner's own began => a same-type
// forward-reference escape, "ordinary execution" per §22.13's own wording
// => do NOT raise the cycle backstop, fall through and let the caller's
// plain load observe the raw not-yet-assigned (null-watermark) value;
// GREATER => some other type's init started (even if it has since
// completed) during the owner's own run => genuine cross-type re-entrancy
// => raise TypeInitializationException as before. (A simpler "innermost
// currently-executing type" marker, tried first, does NOT work: by the
// time the offending read happens the intervening type's own sfinit has
// typically already run to completion — §22.13 itself requires this — so
// its state is DONE, not in-progress, at the read; see the runtime
// global's own comment for the measured counter-example against this
// file's own cross-type control row.) The two static-field READ call sites
// (LlvmBackend_Exprs.cpp) now also call `emitFieldWatermarkGuard` (fix
// #1748's instance-field guard, reused verbatim) after the load, so a
// checked-disposal build raises NullReferenceException AT THE READ that
// observes the watermark.
//
// Because that read happens while still inside the type's own static
// constructor/field-initializer function (emitStaticConstructorBody), the
// raised NullReferenceException unavoidably passes through that function's
// OWN wrap-into-TypeInitializationException propagate block (§22.7's
// general, pre-existing, unrelated-to-this-fix "an exception escaping a
// static initializer is wrapped in TypeInitializationException" rule) before
// an EXTERNAL touch (this file's Main) ever observes it — so the externally
// visible signal is TypeInitializationException.InnerException being the
// NullReferenceException raised "at the read", rather than InnerException
// staying null (the pre-fix signature: the read-site raise WAS ALREADY a
// TypeInitializationException instance, so the wrap block's own "already a
// TIE, do not double-wrap" rule — the exact mechanism
// static_init_cycle_backstop_1553_test.cpp's
// backstop_TIE_cached_directly_inner_null_not_double_wrapped row pins —
// passed the null-inner backstop TIE straight through unchanged).
//
// Mechanically verified FAIL-WITHOUT / PASS-WITH against the FINAL src/
// diff (two debugging passes along the way, both left visible here rather
// than silently smoothed over):
//   (a) an earlier draft of row 1/2's program used a constant-literal `Y`
//       initializer, which folds to the global's own compile-time
//       initializer and never opens a not-yet-assigned window at all
//       (confirmed via `BEZEL_DUMP_IR=1`: `@"bz$M.A$Y" = global ptr
//       @.bz.str.N`, no runtime store anywhere in `bz$A$sfinit`); fixed to
//       a `Make()` method call, mirroring kIntCycle/kStrCycle.
//   (b) the FIRST src/ design (a save/restore-around-sfinit "innermost
//       active type" marker) passed rows 1/2 but FAILED the wide adjacent
//       sweep: static_foldable_forward_ref_1309_test.cpp's 2 of 3 rows
//       regressed, because that fix's `emitStaticPartialReadGuard` change
//       applied the same-type carve-out to MUTABLE static fields too —
//       #1309's own scenario (`b := c + 1;` with `c` mutable) is a
//       same-type forward reference with no readonly-only compile-time
//       check to have escaped, so it has no §22.13 residual and must keep
//       raising unconditionally. Narrowed the fix to `static readonly`
//       fields only (LlvmBackend_Setup.cpp's `isReadonly` check, mirroring
//       the file's own pre-existing `isReadonlyStaticField` from fix
//       #1309) and re-verified both this file and #1309's full sweep green.
// Final cycle: saved this file's diff to a patch, reverted just the src/
// diff (LlvmBackend_Setup.cpp, LlvmBackend_Exprs.cpp, LlvmBackend_impl.hpp,
// bezel_runtime_concurrency.cpp) with `git checkout -- <file>` (never `git
// stash`), rebuilt via `scripts/agent_check.sh --area codegen 4486`: row 1
// (same_type_forward_ref_checked_raises_nre_wrapped_in_tie) FAILED —
// `runChecked(...) == 55` — and row 2
// (same_type_forward_ref_unchecked_does_not_raise_tie) FAILED —
// `runDefault(...) == 255`. Rows 3-5 (the cross-type control and the two
// ordinary same-type controls) PASSED both before and after, as expected —
// none depends on this fix. Restored the fix (`git apply` on the saved
// patch) and rebuilt: all 5 rows PASSED, and the full wide sweep (#1309,
// #1553, #1748, #1391, #917, #720, #796, #1107) stayed green — see the
// commit body for the exact filter list and counts.
//
// Spec: §22.13 "Static initialization order" (v0.306), §22.7 "Constructors"
// / "Observing `this` during construction" residual (v0.306), §31.4
// "Standard exception types".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

int runChecked(const std::string& program) {
    JitResult r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}
int runDefault(const std::string& program) {
    JitResult r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Row 1/2 program: `A.X`'s initializer calls a function VALUE (`_f`, a
// lambda) that reads `A.Y` — a static readonly field of A itself declared
// AFTER X. `_f` is declared and assigned before X (so calling it is not
// itself a direct forward reference), and the lambda BODY reading `A.Y` is
// invisible to StaticInitCycles.cpp's compile-time approximation (§22.13's
// own words: "function values ... contribute nothing" to the tracked call
// set) — so this compiles, matching the spec's own description of the
// escape route.
//
// `Y`'s initializer MUST be a non-constant expression (a static method
// call, exactly mirroring static_init_cycle_backstop_1553_test.cpp's own
// kIntCycle/kStrCycle `Seed()`/`Make()` shape) rather than a bare string
// literal: a literal initializer folds to a compile-time CONSTANT and is
// baked directly into the field's GLOBAL INITIALIZER (never assigned via a
// runtime sfinit store at all — confirmed via BEZEL_DUMP_IR on an earlier,
// defective draft of this row: `@"bz$M.A$Y" = global ptr @.bz.str.N`, no
// `store` to it anywhere in `bz$A$sfinit`), so it is NEVER "not yet
// assigned" and the whole forward-reference window this test exists to
// exercise never opens. `Make()` keeps `Y` genuinely sfinit-stored and
// progress-tracked, matching every other field this file and the existing
// backstop test exercise.
const char* kSameTypeForwardRefProgram =
    "module M; "
    "class A "
    "  private static readonly _f: () -> string := lambda() => A.Y; "
    "  public static readonly X: string := _f(); "
    "  public static readonly Y: string := Make(); "
    "  private static method Make(): string => \"world\"; "
    "end A; "
    "class C public static method Main(): int "
    "  try "
    "    var v := A.X; "
    "    return -1; "
    "  catch var ex: TypeInitializationException => do "
    "    if ex.InnerException is NullReferenceException then return 55; "
    "    else return 1; end; "
    "  end end; "
    "end Main; end C;";

// Row 3 (CONTROL) program: the EXISTING, already-covered CROSS-type backstop
// (static_init_cycle_backstop_1553_test.cpp's kIntCycle) — A.X calls into B,
// which reads back A.Y through a function value: a genuine T -> U -> T
// re-entrancy. Must be UNAFFECTED by this fix: the synthesized backstop
// TypeInitializationException, InnerException still null (no wrap — this IS
// the outermost synthesized exception), exactly as before.
const char* kCrossTypeBackstopProgram =
    "module M; "
    "class A "
    "  public static readonly X: int := B.GetX(); "
    "  public static readonly Y: int := Seed(); "
    "  private static method Seed(): int => 1; "
    "end A; "
    "class B "
    "  private static readonly _f: () -> int := lambda() => A.Y; "
    "  public static method GetX(): int => _f() + 100; "
    "end B; "
    "class C public static method Main(): int "
    "  try var v := A.X; return -1; "
    "  catch var ex: TypeInitializationException => do "
    "    if ex.InnerException = null then return 55; else return 1; end; "
    "  end end; "
    "end Main; end C;";

// Row 4 (CONTROL) program: an ORDINARY same-type static field that reads an
// EARLIER-declared sibling directly (no forward reference at all) — pins
// that the new innermost-init-type push/pop plumbing does not disturb
// correct, everyday same-type static initialization.
const char* kOrdinaryProgram =
    "module M; "
    "class A "
    "  public static readonly Y: string := \"world\"; "
    "  public static readonly X: string := Y + \"!\"; "
    "end A; "
    "class C public static method Main(): int => A.X.Length; "
    "end C;";

}  // namespace

// Row 1: REGRESSION — checked-disposal mode. The proximate exception raised
// "at the read" (§22.13's wording) is NullReferenceException; it reaches
// Main wrapped in TypeInitializationException (§22.7's general, unrelated
// wrap-on-escape rule for any static-initializer-body exception) as
// InnerException, not as a null-inner backstop TIE (the pre-fix signature).
BEZEL_TEST(static_same_type_forward_ref_residual_4486,
           same_type_forward_ref_checked_raises_nre_wrapped_in_tie_spec_22_13) {
    BEZEL_EXPECT_EQ(runChecked(kSameTypeForwardRefProgram), 55);
}

// Row 2: REGRESSION — unchecked (default) build. §22.13: unchecked use of
// the watermark is undefined behavior, NOT a guaranteed
// TypeInitializationException. Pre-fix, emitStaticPartialReadGuard raised
// UNCONDITIONALLY (no checkedDisposal_ gate at all), so even the unchecked
// build hit Main's TypeInitializationException catch (returning 1). Post-fix,
// the same-type case never reaches emitStaticPartialReadGuard's raise (the
// crossType test is false regardless of mode) and emitFieldWatermarkGuard is
// itself checkedDisposal_-gated, so nothing raises: A.X ends up holding the
// watermark null (a benign instance of the permitted undefined behavior for
// this particular program, which never dereferences it), and Main reaches
// its plain `return -1;`. `runAot`'s `exitCode` is `WEXITSTATUS`-masked to an
// UNSIGNED byte (CLAUDE.md's own documented trap): Main's `-1` reads back as
// `255`, not `-1` (confirmed empirically — a temporary debug print during
// this fix's own development showed `ran=1 exitCode=255`), so 255 is the
// correct comparison target for "Main returned -1", not a weakened oracle.
BEZEL_TEST(static_same_type_forward_ref_residual_4486,
           same_type_forward_ref_unchecked_does_not_raise_tie_spec_22_13) {
    BEZEL_EXPECT_EQ(runDefault(kSameTypeForwardRefProgram), 255);
}

// Row 3: CONTROL — genuine cross-type re-entrancy (T -> U -> T) still raises
// the synthesized backstop TypeInitializationException with InnerException
// null, unaffected by this fix.
BEZEL_TEST(static_same_type_forward_ref_residual_4486,
           cross_type_backstop_still_raises_null_inner_tie_spec_22_13) {
    BEZEL_EXPECT_EQ(runChecked(kCrossTypeBackstopProgram), 55);
}

// Row 4: CONTROL — an ordinary (non-forward-referencing) same-type static
// computes the correct value in both modes, unaffected by the new
// innermost-init-type tracking plumbing.
BEZEL_TEST(static_same_type_forward_ref_residual_4486,
           ordinary_same_type_static_init_unaffected_checked_spec_22_13) {
    BEZEL_EXPECT_EQ(runChecked(kOrdinaryProgram), 6);
}
BEZEL_TEST(static_same_type_forward_ref_residual_4486,
           ordinary_same_type_static_init_unaffected_default_spec_22_13) {
    BEZEL_EXPECT_EQ(runDefault(kOrdinaryProgram), 6);
}
