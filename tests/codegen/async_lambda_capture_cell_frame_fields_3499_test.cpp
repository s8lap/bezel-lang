// Codegen — issue #3499 (§27.5 Closures, §30.6 async lambdas, §30.1 `async`
// methods, spec v0.283): `emitAsyncLambda` is the FOURTH state-machine frame
// venue and the only one that does not build its frame through
// `buildStateMachineFrame`, so neither #3466's captured-PARAMETER cell field nor
// #3484's captured-LOCAL pre-boxing field existed there.
//
//     async method Main(): Task<int>
//       var f := async lambda(v: int): Task<int> => do
//         var g := lambda(): int => v;
//         await Task.Delay(1);
//         v := v + 100;
//         return g();            /* prints 7 where §27.5 requires 107 */
//       end;
//
// MECHANISM. `emitAsyncLambda` snapshots its capture set BEFORE the body is
// lowered. At that moment the lambda's own parameter `v` has no `cellVars_`
// entry, so it is classified as a plain by-value lambda param and gets a
// VALUE-typed frame field. The nested `g` then boxes `v` during body lowering,
// rebinding `paramSlots_[v]` to a ptr cell-HOLDER alloca that NO frame field
// describes; the resume entry block re-nulls that holder on every activation
// while the state dispatch jumps straight past the capture site. #3484's
// read-side fall-back then makes the post-suspension read well-defined and
// UNSHARED: the write after the `await` lands in the pre-boxing alloca while the
// closure keeps reading the cell. §27.5: "Modifications to a captured variable
// inside the lambda are visible outside, and vice versa." 7 is not 107, and
// there is no crash and no diagnostic to notice it by.
//
// THE FIX reserves both field kinds in `emitAsyncLambda`'s inline layout, with
// exactly the shapes #3466 and the #3484 follow-up established, and publishes
// them for the RESUME function (`smParamCellField_` / `smLocalPreBoxField_` /
// `smCellFrameFn_`) so `lowerLambda` can wire the holder — and the pre-boxing
// alloca — into `smAllocaToField_`. The existing per-suspend spill/restore then
// carries them, unchanged. Four choices are load-bearing:
//
//   * The new fields are appended AFTER the three trailing async fields
//     (outerTask / pendingTask / spill), so no index any caller already computes
//     moves.
//   * The parameter field is POINTER width, never the parameter's own type
//     (#461 / #808: an 8-byte cell pointer spilled into an `i32` field overflows
//     into its neighbour).
//   * The parameter field is a NEW field, never the parameter's capture field:
//     that one carries the raw VALUE the restore loop copies back on every
//     activation and that `boxLocalSlot` reads to seed the cell. One slot cannot
//     carry both meanings.
//   * The LOCAL field carries the local's PRECISE pre-boxing type, not `ptr`.
//     A record local spilled through a pointer-width field was measured leaking
//     raw stack into the program's answer when the #3484 follow-up made the same
//     choice for the method venue.
//
// THE STALE-SET HAZARD the issue named is real and handled: `computeFrameCaptured
// Locals` runs only for a BLOCK-bodied lambda, so an expression-bodied one would
// have keyed frame LAYOUT off the ENCLOSING body's sets. They are cleared on that
// path. (A stale entry could not name one of THIS lambda's own parameters — the
// symbols differ — so the surviving hazard was a reserved-but-unused field rather
// than a mis-indexed one; a layout decision must not depend on it either way.)
//
// §13-OF-THE-LANE-RULES / issue 3505: the three members published here are all
// cleared by `clearSmFrame` AND saved/restored around both nested-lambda
// emissions in `lowerLambda`, so no new half-contract is introduced. The nested-
// `async lambda`-between-capture-and-read shape is pinned below as a regression
// guard, because that is the exact shape a missing save/restore breaks.
//
// DIRECTION OF DANGER. Layout-side only: it reserves frame fields and makes an
// existing spill/restore see two more allocas. No free is widened, no lifetime
// shortened, no release added, so no row below claims to be an over-release
// guard. An unused reserved field costs 8 bytes in a frame that is already
// per-call heap-allocated.
//
// FAIL-WITHOUT / PASS-WITH, measured one program at a time on one build of each
// compiler. `ef339fa` is the parent (both #3484 fixes and #3466, no #3499):
//
//   row                                        ef339fa                this
//   write the param after the suspension       7                      107
//   the same, x500                             3500 + 2000 B leaked   53500, clean
//   async-lambda LOCAL, conditional capture    0 then SEGV @ 0        0 2 8 6 16 10
//   control: the same shape with no lambda     0 2 8 6 16 10          unchanged
//   control: local captured unconditionally    11                     unchanged
//   control: expression-bodied async lambda    8                      unchanged
//   guard: nested async lambda between the two 0 1 4 3                unchanged
//
// per §27.5, §30.6, §30.1 (issue #3499)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The VALUE printed is the oracle for the rows whose programs still carry an
// unrelated, still-open leak: a local whose capture site sits behind an `if`
// takes boxLocalSlot's GUARDED path and its cell is never reclaimed — measured
// at 2000 bytes over 500 invocations in a purely SYNCHRONOUS method, identical
// on this tree and on `ef339fa`, and filed as #3508. A holder that did not
// survive the suspension either prints a specific wrong number or dies at
// address 0, and "<did-not-run>" is a distinguishable, failing value.
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// FAIL-WITHOUT rows
// ═══════════════════════════════════════════════════════════════════════════

// The issue's own program. §27.5's sharing guarantee across the async lambda's
// own suspension: the write to `v` AFTER the `await` must be visible through the
// closure that captured it BEFORE. Without the parameter cell field the write
// lands in #3484's fall-back pre-boxing alloca while `g` keeps reading the cell —
// 7, silently, where the answer is 107.
// per §27.5, §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           async_lambda_write_own_param_after_suspension_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(v: int): Task<int> => do "
        "      var g := lambda(): int => v; "
        "      await Task.Delay(1); "
        "      v := v + 100; "
        "      return g(); "
        "    end; "
        "    Console.WriteLine((await f(7)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "107\n");
}

// The same program driven x500, under the full memory net. Two independent
// signals move together here and neither is visible one-shot: the VALUE goes
// 3500 -> 53500 (500 x 7 vs 500 x 107), and `ef339fa` drops one 4-byte cell per
// invocation — 2000 bytes over 500 allocations — because the re-nulled holder
// makes the scope-end cell free reclaim null while the real cell is orphaned.
// A single iteration of this program is LSan-CLEAN on both trees; the row exists
// at x500 for exactly that reason.
// per §27.5, §28.9, §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           async_lambda_param_cell_reclaimed_x500_spec_27_5_28_9) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(v: int): Task<int> => do "
        "      var g := lambda(): int => v; "
        "      await Task.Delay(1); "
        "      v := v + 100; "
        "      return g(); "
        "    end; "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + (await f(7)); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "53500\n");
}

// The LOCAL half of the same venue. A CONDITIONALLY executed capture site inside
// the async lambda's own body: on the odd inputs the capture never runs, the
// holder is legitimately null, and #3484's fall-back addresses the pre-boxing
// alloca — which this venue reserved no field for, so `boxedStorage` DECLINED the
// fall-back and the read stayed a null dereference. `ef339fa` prints 0 and then
// dies at address 0 on n = 1. The expected sequence is the arithmetic identity
// s + k for k = 2n: 0, 2, 8, 6, 16, 10.
// per §27.5, §30.6, §30.1 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           async_lambda_conditional_local_capture_prebox_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(n: int): Task<int> => do "
        "      var k := n * 2; "
        "      var s := 0; "
        "      if n mod 2 = 0 then "
        "        var g := lambda(): int => k; "
        "        s := g(); "
        "      end if; "
        "      await Task.Delay(1); "
        "      return s + k; "
        "    end; "
        "    var i := 0; "
        "    while i < 6 do "
        "      Console.WriteLine((await f(i)).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n2\n8\n6\n16\n10\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLS AND ONE REGRESSION GUARD — green on `ef339fa` as well
// ═══════════════════════════════════════════════════════════════════════════

// The row above with the lambda DELETED and `s := k` in its place. It computes
// the same six numbers by construction, so it is the arithmetic oracle for what
// the capturing version must print — without it, "0 2 8 6 16 10" would be a
// number this file asserts rather than a number the language requires.
// per §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           control_async_lambda_conditional_no_capture_spec_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(n: int): Task<int> => do "
        "      var k := n * 2; "
        "      var s := 0; "
        "      if n mod 2 = 0 then "
        "        s := k; "
        "      end if; "
        "      await Task.Delay(1); "
        "      return s + k; "
        "    end; "
        "    var i := 0; "
        "    while i < 6 do "
        "      Console.WriteLine((await f(i)).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n2\n8\n6\n16\n10\n");
}

// An UNCONDITIONAL capture of an async-lambda body local, written after the
// suspension. This already worked: the capture site dominates the read, so the
// holder is never observed null and the moved frame field carries it. It is here
// to pin that reserving the pre-boxing field does not disturb the dominated path
// — the one place a second field for the same local could have gone wrong.
// k = 6, captured; after the await k = 11; g() = 11.
// per §27.5, §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           control_async_lambda_unconditional_local_capture_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(n: int): Task<int> => do "
        "      var k := n * 2; "
        "      var g := lambda(): int => k; "
        "      await Task.Delay(1); "
        "      k := k + 5; "
        "      return g(); "
        "    end; "
        "    Console.WriteLine((await f(3)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "11\n");
}

// An EXPRESSION-bodied async lambda. `computeFrameCapturedLocals` does not run
// for one, which is the stale-set hazard the issue named: before the fix the sets
// still described the enclosing frame body when the field reservation read them.
// This row pins that clearing them leaves the expression-bodied path emitting the
// same program it always did. 7 + 1 = 8.
// per §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           control_expression_bodied_async_lambda_spec_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(v: int): Task<int> => v + (await Task.FromResult(1)); "
        "    Console.WriteLine((await f(7)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n");
}

// REGRESSION GUARD, not evidence. A nested `async lambda` sits BETWEEN a
// conditional capture and the read of the captured variable in the ENCLOSING
// async method. `emitAsyncLambda`'s frame scope runs `clearSmFrame` on the way
// out, so any member this file publishes there must also be saved and restored
// around the nested emission — the two halves of the one contract issue 3505
// names. All three published members already are, and this shape is how a
// missing half shows up: the #3484 follow-up regressed exactly it, from the
// correct 0 1 4 3 to SEGV @ 0, while fixing something else. Green on `ef339fa`
// and green here.
// per §27.5, §30.1, §30.6 (issue #3499)
BEZEL_TEST(async_lambda_capture_cell_frame_fields_3499,
           guard_nested_async_lambda_between_capture_and_read_spec_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Step(v: int): Task<int> "
        "    var s := 0; "
        "    if v mod 2 = 0 then "
        "      var g := lambda(): int => v; "
        "      s := g(); "
        "    end if; "
        "    var h := async lambda(): Task<int> => do await Task.Delay(1); return 0; end; "
        "    await Task.Delay(1); "
        "    return s + v + (await h()); "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 4 do "
        "      Console.WriteLine((await Step(i)).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n1\n4\n3\n");
}
