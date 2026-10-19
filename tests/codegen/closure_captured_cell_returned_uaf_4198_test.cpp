// Codegen: a captured closure-typed local independently RETURNED BY NAME
// from the enclosing function is a genuine heap-use-after-free (issue #4198).
//
// WHAT THE ISSUE CLAIMED. #4198 was filed as a defect specific to a MUTABLY
// captured closure-typed local reassigned inside a nested lambda:
//
//     var f: () -> int := lambda() => 0;
//     var mutate := lambda() => do f := lambda() => 42; end;
//     mutate();
//     return f;                 // <- heap-use-after-free, freed by envdtor
//
// It further claimed a READ-ONLY capture (never reassigned) is unaffected:
// "this needs the mutation ... being exercised". Investigation (this fix)
// found that claim is WRONG: a captured local that is never written at all,
// but is independently RETURNED BY NAME while a non-escaping lambda still
// merely reads it, hits the IDENTICAL crash signature with no write anywhere
// in the program:
//
//     var f: () -> int := lambda() => 7;
//     var reader := lambda() => do var x := f(); end;
//     reader();
//     return f;                 // <- heap-use-after-free, freed by envdtor
//
// ROOT CAUSE. A captured local's own scope-end release is suppressed whenever
// it is captured at all — codegen's "first capture transfers the producer's
// suppressed release to the envdtor" model (§27.5 / §28.9 / #672). That model
// assumes the envdtor is the ONLY remaining consumer of the cell's content.
// `return x;` is a SECOND, independent consumer: it hands the raw reference to
// the caller with no retain (the ordinary "return a local, transfer, don't
// release" idiom — the only reason a plain, UNcaptured closure return works at
// all). When the same local is BOTH captured AND returned, the envdtor's
// content release and the return's silent transfer both claim the SAME single
// reference: the envdtor frees it during THIS frame's own scope-end cleanup
// (which runs after the return value is computed but before control reaches
// the caller), so the caller receives an already-dangling pointer.
//
// This is a SEPARATE hazard from #4174 (closure-local reassignment losing the
// displaced value — a leak, fixed via `localSlot()`-gated
// lowerClosureLocalReassign) and from #2706 (a cell WRITTEN and shared by
// MORE THAN ONE env — fixed via `mutatedSharedCaptureCell`). Neither reaches
// this shape: #4174's fix (not on this tree — see below) is gated on
// `localSlot(target)`, which is null for a captured cell; #2706's own
// existing guard test (`sole_non_loop_capture_of_written_local_stays_balanced
// _spec_28_9`, lambda_captured_string_cell_write_2706_test.cpp) is
// deliberately the case that stays BALANCED — the captured local there is
// never independently returned, only read via `.Length` inside the SAME
// function, which does not need the cell's identity to survive past the
// function.
//
// BASELINE. Measured on this run's integration branch (r18-integration,
// commit 53f8aa0) — NOT on origin/main directly, and NOT with #4174's fix
// present: `git log`/`grep` confirmed neither `lowerClosureLocalReassign` nor
// `retainAssignedClosureParams` (#4174's own machinery) exists in this tree,
// so the two are independent. Both crash identically on this exact base.
//
// THE FIX, AND WHY IT LEAKS ON PURPOSE. `Emitter::returnedCaptureVars_`
// (LlvmBackend_impl.hpp) records every LOCAL DECL / PARAMETER SYMBOL directly
// returned by name from the enclosing function (collectBoxCaptureLocals's new
// `scanReturn`, which does NOT descend into lambda bodies — a nested lambda's
// own `return` escapes THAT lambda, not this function). `lowerLambda`'s two
// capture loops (captured PARAMS and captured LOCALS, LlvmBackend_Exprs2.cpp)
// now OR `capturedLocalReturnedByName(key, semaTy)` into the existing
// `mutatedSharedCaptureCell(...)` suppression decision — independent of
// `assignedCaptureVars_` / `singleCaptureLocals_` / `loopStack_`, since the
// hazard has nothing to do with mutation or how many envs share the cell:
// keying the envdtor's release to "what this env captured" is impossible once
// the LOCAL's own identity independently escapes, exactly as #2706's own
// comment already argues for the written-and-shared case. So a bounded leak
// in place of a use-after-free — the SAME prime-directive trade #2706 already
// made — not a new policy.
//
// NOT FIXED HERE (filed separately, out of #4198's own stated scope): issue
// #4227, the displaced-value leak on a WRITE THROUGH A CAPTURED CELL
// (`f := lambda() => 42;` never releases the OLD `lambda() => 0`) — the
// captured-cell twin of #4174, which is `localSlot()`-gated and does not
// reach this lvalue path either way. Rows A/C/D below carry this KNOWN,
// deliberate leak and use `runAotCheckedLenient` (ASan on, LeakSanitizer
// off) accordingly — see the per-row comment for the measured byte count.
// Issue #4226: a DEEPER hazard (the captured local ALSO captured
// transitively by a lambda nested two levels down) still crashes identically
// before and after this fix.
//
// §27.5 ("Closures" — "Modifications to a captured variable inside the lambda
// are visible outside, and vice versa") and §28.1 ("a function type ... may
// not carry `owned`") are the only genuine citations here: §28.9 explicitly
// disclaims the refcounting IMPLEMENTATION ("The implementation of
// runtime-managed types' lifecycle ... is not part of this specification"),
// so this is an INTERNAL soundness fix, not a spec rule being implemented —
// matching #4174's own follow-up's citation correction (issue 4177's class).

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAot / runAotChecked(Lenient) — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

}  // namespace

// =====================================================================
// (A) The issue's own repro: mutably captured, reassigned inside the
// capturing lambda, then returned. Pre-fix: heap-use-after-free, freed by
// bz$lam$N$envdtor, in main (this measured trace matches the issue's own
// verbatim). Post-fix: correct value (42), with a KNOWN 40-byte/1-allocation
// leak from the SEPARATE, unfixed displaced-value defect described above —
// runAotCheckedLenient (ASan on, LSan off) is the correct oracle for that
// reason, not a weaker check.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           mutated_single_capture_returned_no_crash_correct_value_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 42; "
        "    end; "
        "    mutate(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free in main
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (A2) The same shape, loop-amplified to 500 calls — the issue's own "500x
// amplification, non-flaky, first iteration" claim, re-verified under THIS
// fix. Deliberately Lenient (the per-call displaced-value leak scales to
// 20000 bytes / 500 allocations, which is expected, not a regression) — this
// row asserts CRASH-FREEDOM under repetition, not leak-freedom.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           mutated_single_capture_returned_x500_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 42; "
        "    end; "
        "    mutate(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      acc := acc + g(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 21000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (B) READ-ONLY capture, returned, NO mutation anywhere — the case the issue
// itself claimed was unaffected ("this needs the mutation ... being
// exercised"). REFUTES that claim: identical heap-use-after-free pre-fix
// (measured), fully ASan+LSan clean post-fix (no write anywhere means no
// displaced-value leak either, so runAotChecked — LSan ON — is the correct,
// stronger oracle here, unlike row A).
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           read_only_capture_returned_no_mutation_fully_clean_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 7; "
        "    var reader := lambda() => do "
        "      var x := f(); "
        "    end; "
        "    reader(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free in main
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// =====================================================================
// (B2) Same read-only-capture-then-return shape, loop-amplified to 500 calls
// under the FULL LeakSanitizer oracle (not Lenient) — the strongest claim in
// this file: zero leak, zero crash, at scale, for the shape the issue itself
// said needed no fix.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           read_only_capture_returned_x500_is_memory_clean_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 7; "
        "    var reader := lambda() => do "
        "      var x := f(); "
        "    end; "
        "    reader(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var acc := 0; "
        "    while i < 500 do "
        "      var g := C.MakeClosure(); "
        "      acc := acc + g(); "
        "      i := i + 1; "
        "    end; "
        "    if acc <> 3500 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (C) Reassigned TWICE inside the capturing lambda before return — varies the
// "reassigned once vs twice" axis. Two displaced values now leak (the SAME
// pre-existing, separately-tracked defect as row A, just doubled: 80 bytes /
// 2 allocations) — Lenient again for that reason. The crash-freedom and
// final-value correctness are what this row certifies.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           reassigned_twice_before_return_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 1; "
        "      f := lambda() => 42; "
        "    end; "
        "    mutate(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free in main
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (D) PARITY CONTROL — two envs share the cell (one mutates, one only
// reads), and the local is returned. This shape was ALREADY safe before this
// fix: #2706's own `mutatedSharedCaptureCell` already suppresses the content
// release once MORE THAN ONE env can share a WRITTEN cell, independent of
// return status, so this row passes IDENTICALLY on both sides of THIS fix —
// verified mechanically (see commit body). Included so the two mechanisms'
// boundary is pinned by a test, not just by reasoning.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           multi_capture_one_mutates_one_reads_returned_parity_spec_28_9) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 42; "
        "    end; "
        "    var reader := lambda() => do "
        "      var x := f(); "
        "    end; "
        "    mutate(); "
        "    reader(); "
        "    return f; "
        "  end MakeClosure; "
        "  public static method Main(): int "
        "    var g := C.MakeClosure(); "
        "    return g(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (E) OVER-CORRECTION GUARD — single capture, mutated, called 9 times, but
// the local is NEVER returned (only invoked). `capturedLocalReturnedByName`
// must NOT fire here (returnedCaptureVars_ is empty for this program), so
// this row's behaviour is UNCHANGED by this fix — verified mechanically
// (identical 360-byte/9-allocation leak — the SAME pre-existing
// displaced-value defect as row A, once per call — measured on both sides).
// Fails only if the fix over-reaches into the "not returned" shape.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_returned_uaf_4198,
           single_capture_mutated_not_returned_stays_crash_free_spec_28_9) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var mutate := lambda() => do "
        "      f := lambda() => 42; "
        "    end; "
        "    mutate(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 378 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
