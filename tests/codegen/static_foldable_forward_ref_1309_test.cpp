// fix #1309 / §22.13 / §22.7: a MUTABLE static field whose initializer is a
// foldable (constant-literal) expression must still initialize at RUNTIME in
// textual declaration order — it may NOT have its value baked into the backing
// global at compile time. §22.13: "Within a single type, static fields are
// initialized in textual declaration order," and "Partially-initialized statics
// are never observable" (the same-thread re-entrancy backstop raises a
// TypeInitializationException rather than exposing an unassigned field).
//
// Before this fix a mutable static's literal initializer was compile-time-baked
// (constInitFor), so an EARLIER (textually) mutable static that forward-
// referenced it silently observed the baked value — bypassing both the textual
// init order AND the partial-state read guard (the guard was inert for baked
// fields). The compiler's two paths DISAGREED: the foldable case returned the
// folded value (rc=6 below), while the same program with a non-foldable
// initializer correctly raised. The two paths now agree: both raise.
//
// The repro shape (issue #1309, prose treated as a hint only — its claimed
// rc=1/default-observation outcome contradicts §22.13's "never observable" rule;
// the spec-mandated behavior is the raise, matching the non-foldable path):
//   class Cfg
//     public static b: int := c + 1;   // b declared BEFORE c — forward ref
//     public static c: int := 5;        // foldable literal
//   end;
// b's initializer runs first (textual order); reading c (not yet initialized)
// triggers the §22.13 re-entrancy backstop → TypeInitializationException.
//
// These are codegen tests (static-init timing + foldable-init routing), so runJit
// is the right harness — the exit code captures the caught-exception outcome.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §22.13: a MUTABLE static (b) whose foldable initializer `c + 1` forward-
// references a later mutable static (c) must raise the re-entrancy backstop —
// not silently observe c's baked value. The catch confirms the exception is
// raised AND that InnerException is null (the §22.13 cycle-backstop signature,
// §31.4). Before the fix Main returned 6 (the folded `5 + 1`, the bug).
BEZEL_TEST(static_foldable_forward_ref_1309,
           foldable_forward_ref_raises_TIE_not_baked_value_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cfg "
        "  public static b: int := c + 1; "   // forward ref to a later static
        "  public static c: int := 5; "        // foldable literal initializer
        "end Cfg; "
        "class C public static method Main(): int "
        "  try "
        "    var v := Cfg.b; "
        "    return v; "                        // pre-fix: 6 (baked value observed)
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException = null then return 55; else return 1; end; "
        "  end end; "
        "end Main; end C;"), 55);
}

// §22.13 regression guard: a BACKWARD reference (c declared first, then b reads
// c) is unaffected — c is already stored when b's initializer runs, so b observes
// the real value 6 and no backstop raises. Pins that the fix narrowed the
// behavior to forward references only and did not break ordinary textual-order
// init of foldable mutable statics.
BEZEL_TEST(static_foldable_forward_ref_1309,
           backward_ref_foldable_unaffected_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cfg "
        "  public static c: int := 5; "         // c first
        "  public static b: int := c + 1; "     // backward ref — c already stored
        "end Cfg; "
        "class C public static method Main(): int => Cfg.b; "  // 6
        "end C;"), 6);
}

// §22.13: the foldable and non-foldable forward-reference paths must AGREE. The
// same forward-reference shape with a NON-foldable initializer (c := Compute())
// already raised correctly (it was never baked); the foldable form now matches
// it. This is the "compiler's two paths disagree" smoking gun from #1309 — both
// raise the backstop with InnerException null.
BEZEL_TEST(static_foldable_forward_ref_1309,
           foldable_and_nonfoldable_forward_ref_agree_spec_22_13) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cfg "
        "  private static method Compute(): int return 5; end Compute; "
        "  public static b: int := c + 1; "     // forward ref, non-foldable c
        "  public static c: int := Compute(); "
        "end Cfg; "
        "class C public static method Main(): int "
        "  try "
        "    var v := Cfg.b; "
        "    return v; "
        "  catch var ex: TypeInitializationException => do "
        "    if ex.InnerException = null then return 55; else return 1; end; "
        "  end end; "
        "end Main; end C;"), 55);
}
