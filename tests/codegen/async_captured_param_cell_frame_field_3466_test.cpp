// Codegen — issue #3466 (§27.5 Closures, §29.3 Iterator methods, §30.1 `async`
// methods, spec v0.283): a lambda-captured PARAMETER's capture-CELL HOLDER gets
// no state-machine frame field, so it reads back null after a suspension.
//
// §27.5 boxes a captured variable into a heap cell and rebinds its slot to a
// ptr-typed HOLDER alloca. In a state-machine body that holder is an alloca of
// the RESUME function, null-initialised in its ENTRY block — which re-runs on
// every activation, while the state dispatch jumps straight past the capture
// site. So on the second and later activations the holder is null:
//
//     async method Run(v: int): Task<int>
//       var f := lambda(x: int): int => x + v;
//       await Task.Delay(1);
//       return f(3) + v;                /* SIGSEGV in bz$async$Run$Resume */
//
// `lowerLambda` moves `smAllocaToField_` onto the new holder only when the
// ORIGINAL slot already had a frame field. A LOCAL has one — collectSuspend
// Locals reserves it and #808 widens it to ptr precisely because the slot now
// holds a cell POINTER. A PARAMETER has none: its cross-suspension restore is
// the resume prologue's unconditional copy from its CAPTURE field into the
// pre-boxing alloca, and that copy knows nothing about the holder the capture
// just rebound `paramSlots_` to. So the parameter's holder inherited nothing
// and was never spilled.
//
// THE FIX gives it a field of its own. `buildStateMachineFrame` reserves one
// POINTER field per lambda-captured parameter — pointer-width by construction
// (#808 / #461: an 8-byte cell pointer spilled into an `i32` field overflows
// into its neighbour), and appended LAST, after `extraTrailing`, so no index any
// caller already computes from captureStart / localStart / currentValField
// moves. The three body-lowering sites (async resume, iterator MoveNext,
// async-iterator resume) publish the map, and `lowerLambda` wires the holder
// into `smAllocaToField_`. The existing per-suspend spill/restore then carries
// it, unchanged. Sharing the parameter's own capture field is NOT an option:
// that one holds the raw VALUE the prologue restores on every activation and
// that `boxLocalSlot` seeds the cell from.
//
// WHAT THE ROWS OBSERVE, and why they are not #3484's rows. #3484's read-side
// repair (a null holder falls back to the pre-boxing alloca) already stops the
// CRASH here, so a row that only READS the parameter after the await passes with
// #3484 alone — it is kept below as a control, not as this fix's evidence. What
// needs THIS fix is §27.5's actual guarantee across the suspension —
// "Modifications to a captured variable inside the lambda are visible outside,
// and vice versa." With the holder unrestored, a write lands in the fall-back
// slot while the closure keeps reading the cell (or the reverse), and the
// program prints a WRONG NUMBER with no crash and no diagnostic. Both directions
// are rows below, and both were measured at 7 where §27.5's answer is 107.
//
// DIRECTION OF DANGER. Layout-side only: it reserves frame fields and makes an
// existing spill/restore see one more alloca. No free is widened, no lifetime
// shortened, no release added — so there is no over-release direction to guard
// and no row below claims to be such a guard.
//
// WHAT IS NOT FIXED HERE. The issue's part (b) is a 2000 B / 500 leak, whose
// stated mechanism is that "the scope-end cell free frees null". On this tree
// there is no scope-end free of a guarded parameter cell to feed at all —
// `lowerLambda`'s admission excludes it, which is #3403's territory — so the
// leak survives this fix and these are `runAot` VALUE rows, not `runAotChecked`
// leak rows.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`). Two "before" columns, because these
// rows sit on top of #3484: `0c5ecd1` is the tree with NEITHER fix, `+#3484` is
// the tree with the read-side repair and WITHOUT this frame field — which is the
// column that decides whether a row is evidence for THIS fix or a control.
//
//   row                                       0c5ecd1     +#3484     +this
//   write the param AFTER the suspension      SEGV @ 0    7          107
//   read a param written BEFORE the suspend   SEGV @ 0    7          107
//   iterator, write between two `yield`s      SEGV @ 0    9          14
//   read the param after the `await`          SEGV @ 0    23         23
//   async + conditional capture, combined     SEGV @ 0    280        280
//   control: lambda declared AFTER the await  23          23         23
//
// The first three are this fix's FAIL-WITHOUT rows; the last three pass with
// #3484 alone and are kept as CONTROLS, not as evidence. That split is the honest
// reading of the measurement, not a presentational choice: #3466 as filed has a
// SIGSEGV half and a leak half, and the SIGSEGV half turns out to be answered by
// #3484's read-side fallback — a null holder falls back to the pre-boxing alloca,
// which the resume prologue restores from the capture field on every activation,
// so the VALUE the crash rows print is right without any frame field at all.
// What the frame field alone answers is the SHARING, which is why every row that
// discriminates it is a wrong-NUMBER row rather than a crash row. Calling the
// crash rows this fix's evidence would have been a claim the probe contradicts.
//
// The iterator row is red in both earlier columns for two different reasons: on
// `0c5ecd1` it dies at address 0 inside bz$iter$Gen$MoveNext, and with #3484 it
// survives but prints 9 — the fallback makes the read well-defined and still
// unshared, which is exactly the distinction this file is about.
//
// per §27.5, §29.3, §30.1 (issue #3466)

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// These shapes leak their capture cell by a separate, still-open defect
// (#3403 / #3435 family), so LeakSanitizer is not the oracle here. The oracle
// is the exact VALUE printed: a holder that did not survive the suspension
// produces a specific, different wrong number.
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #3466 — the holder needs a state-machine frame field of its own
// ═══════════════════════════════════════════════════════════════════════════

// CONTROL — the issue's part (a), verbatim. On `0c5ecd1` this is a SIGSEGV
// inside bz$async$Run$Resume, because `+ v` after the suspension dereferences the
// re-nulled holder; #3484's read-side fallback already makes it print 23. Kept
// because it is the issue's own program and because it pins that the frame field
// does not REGRESS it. f(3) + v = (3 + 10) + 10 = 23.
// per §27.5, §30.1 (issue #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           async_read_captured_param_after_await_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(v: int): Task<int> "
        "    var f := lambda(x: int): int => x + v; "
        "    await Task.Delay(1); "
        "    return f(3) + v; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(10)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "23\n");
}

// SHARING across the suspension, write-after direction. §27.5: "Modifications
// to a captured variable inside the lambda are visible outside, and vice versa"
// — so the write to `v` AFTER the await must be visible through the closure that
// captured it BEFORE it. This row discriminates the frame-field fix: with only
// the #3484 read-side repair the write lands in the fall-back pre-boxing alloca
// while the closure still reads the cell, and the program prints 7 instead of
// 107 — a WRONG ANSWER with no crash and no diagnostic.
// per §27.5, §30.1 (issue #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           async_write_captured_param_after_suspension_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(v: int): Task<int> "
        "    var f := lambda(): int => v; "
        "    await Task.Delay(1); "
        "    v := v + 100; "
        "    return f(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(7)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "107\n");
}

// The same sharing obligation in the other direction: the write happens BEFORE
// the suspension (so it reaches the real cell) and the READ happens after. With
// the holder unrestored the read falls back to the pre-boxing alloca, which the
// resume prologue has just re-restored from the capture field — the ORIGINAL
// argument — so it prints 7. The two directions fail through different code
// (a store vs a load) and both must be right.
// per §27.5, §30.1 (issue #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           async_read_captured_param_written_before_suspension_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(v: int): Task<int> "
        "    var f := lambda(): int => v; "
        "    v := v + 100; "
        "    await Task.Delay(1); "
        "    return v; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(7)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "107\n");
}

// The ITERATOR spelling. A `yield return` suspends exactly as an `await` does
// and MoveNext opens with the same state-dispatch switch, so the holder is
// re-nulled per advance for the same reason. `v := v + 5` between two yields
// must reach the closure: 3 + 3 + 8 = 14. Pre-fix the write never reached the
// cell and the third yield printed the stale 3, for a total of 9.
// per §27.5, §29.3 (issue #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           iterator_captured_param_shared_across_yield_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v; "
        "    yield return f(); "
        "    yield return v; "
        "    v := v + 5; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var x in C.Gen(3) do acc := acc + x; end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "14\n");
}

// CONTROL — both shapes in one program: the capture site is conditional (so the
// holder is null on half the calls — #3484) AND the read is after a suspension.
// It was a SIGSEGV before #3484 and prints 280 with #3484 alone, so it is a
// PARITY row for this fix, not evidence for it: the read after the `await` is
// satisfied by the fall-back to the pre-boxing alloca, whose value the resume
// prologue restores. An earlier draft of this comment claimed neither repair
// alone makes it green; the probe disagreed and the claim is corrected here
// rather than left standing.
// Step(v) = (v even ? v : 0) + v over 0..19 = 180 + 100 = 280.
// per §27.5, §30.1 (issues #3484, #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           async_conditional_capture_read_after_await_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Step(v: int): Task<int> "
        "    var s := 0; "
        "    if v mod 2 = 0 then "
        "      var g := lambda(): int => v; "
        "      s := g(); "
        "    end if; "
        "    await Task.Delay(1); "
        "    return s + v; "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var acc: int := 0; "
        "    for var i in 0..<20 do acc := acc + await Step(i); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "280\n");
}

// CONTROL — the issue's own discriminator: with the lambda declared AFTER the
// `await`, the capture site and every read of the parameter are in the SAME
// resume activation, so the holder is never observed across a suspension. Green
// on the pre-fix compiler; it is what makes the four rows above tests of the
// SUSPENSION rather than of captured parameters generally.
// per §27.5, §30.1 (issue #3466)
BEZEL_TEST(async_captured_param_cell_frame_field_3466,
           control_async_lambda_declared_after_await_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(v: int): Task<int> "
        "    await Task.Delay(1); "
        "    var f := lambda(x: int): int => x + v; "
        "    return f(3) + v; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(10)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "23\n");
}
