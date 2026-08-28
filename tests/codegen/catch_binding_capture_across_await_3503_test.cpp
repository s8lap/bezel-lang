// Codegen — issue #3503 (§27.5 Closures, §30.9 the async fault boundary,
// §31.2 `catch` arms, §30.1 `async` methods, spec v0.283): a lambda that
// captures a CATCH-ARM BINDING in an arm that also `await`s read the binding
// back through a NULL capture-cell holder after the suspension.
//
//     async method Run(n: int): Task<int>
//       try   … throw …
//       catch var e: Exception => do
//         var g := lambda(): int => e.Message.Length;
//         await Task.Delay(1);
//         acc := g() + e.Message.Length;      /* SIGSEGV @ 0 */
//       end
//       end try;
//
// MECHANISM. §27.5 boxes a captured variable into a heap cell and rebinds its
// slot to a ptr-typed HOLDER alloca. `lowerLambda` keeps that holder alive
// across a suspension by MOVING the original slot's state-machine frame field
// onto it — but a catch-arm binding has no such field to move. `collectSuspend
// Locals` never sees it: `lowerTry` creates the binding's slot directly, and its
// cross-suspension storage is the handler SPILL AREA instead (`lowerTry`
// registers that alloca in `asyncHandlerSpillSlots_` when the arm contains an
// `await`, §30.9 / §31.2, and `promoteDemotedSlotsToFrame` moves it into the
// frame's calloc'd spill area). The HOLDER was in NEITHER place: a bare
// entry-block alloca of the resume function, whose `store ptr null` re-runs on
// every activation while the state dispatch jumps straight past the capture
// site. So the last thing written to it before the read was null.
//
// This is #3466's family with a THIRD storage class — a captured local gets a
// frame field, a captured parameter now gets one of its own (#3466), and a
// captured catch-arm binding got neither. #3484's read-side fall-back
// deliberately DECLINES this shape, because the fall-back address would be the
// promoted binding alloca, which promotion erases; declining kept the loud
// SIGSEGV rather than a plausible wrong number.
//
// THE FIX gives the holder the SAME home the binding itself has: it is
// registered in `asyncHandlerSpillSlots_` alongside it, keyed on the ORIGINAL
// slot's own membership, so this fires exactly where the existing authority has
// already decided "this slot must survive a suspension and has no frame field".
// The spill area is calloc'd, so the holder starts null — the guard's first-pass
// state — and then PERSISTS across resumes, which is the repair. Two consequences
// were load-bearing and are pinned by rows below:
//
//   * `promoteDemotedSlotsToFrame` must ERASE the holder's entry-block null-init.
//     Not only because that store would reference a GEP defined after the
//     dispatch split (malformed IR), but because re-nulling the holder on every
//     activation is precisely the defect. A slot that still carries an
//     entry-block use after that erase is DECLINED, keeping its pre-existing
//     loud failure instead of IR that verifies and addresses the wrong storage.
//
//   * A catch-arm binding is RE-BOUND on every entry to its arm, exactly like a
//     `case`-arm BindingPattern, so a GUARDED capture cell (allocated once, then
//     reused) must be refreshed from the binding slot at each capture. Without
//     that, a `try`/`catch` inside a loop reads iteration 1's exception object —
//     which the arm's own disposal has already reclaimed — through the closure.
//     That half is PRE-EXISTING and reproduces on a purely SYNCHRONOUS method
//     (row `sync_reentered_catch_arm_refreshes_cell`); it becomes load-bearing here
//     because making the holder survive a resume would otherwise have converted
//     the async spelling's SIGSEGV into that use-after-free.
//
// DIRECTION OF DANGER. Layout-side: one more alloca reaches an existing
// heap-spill promotion, and one extra store refreshes a cell the same code
// already allocates. No free is widened, no lifetime shortened, no release
// added. The free-side admission (`guardedOk` in lowerLambda, which lets a
// case-arm binding's guarded cell be reclaimed at the arm scope even inside a
// loop) is deliberately NOT widened to catch arms — that is a release-direction
// change and needs its own evidence — so the in-loop shape still leaks one cell
// per invocation (#3508's family), which is why its row is a value/ASan row and
// not an LSan one.
//
// NOT FIXED HERE, filed as #3506: a CONDITIONALLY executed capture site inside
// an awaiting catch arm still dereferences a null holder (the capture never ran,
// and #3484's fall-back is still declined because the fall-back slot is the
// erased binding alloca). That shape is `SEGV @ 0` on `ef339fa` and on this
// commit alike — unchanged, not regressed.
//
// FAIL-WITHOUT / PASS-WITH, measured one program at a time on one build of each
// compiler. `ef339fa` is the parent (both #3484 fixes, no #3503):
//
//   row                                        ef339fa                this
//   catch binding captured across in-arm await SEGV @ 0                8 / 1
//   two awaits in the arm                      SEGV @ 0                15
//   the arm lives inside an async LAMBDA       SEGV @ 0                8
//   the arm lives in an async ITERATOR         SEGV @ 0                3 / 6
//   two lambdas on one binding, x500           16500 + 4000 B leaked   16500, clean
//   nested try, both arms capture, x500        21000 + 8000 B leaked   21000, clean
//   SYNC, arm re-entered in a loop             heap-use-after-free     6
//   control: arm awaits, NO lambda             8                       8
//   control: lambda in the arm, await OUTSIDE  8                       8
//   control: async, arm re-entered in a loop   6 (24 B / 3 leaked)     6 (8 B / 1)
//
// ⚠ The SYNC re-entered-arm row needs the ASan oracle specifically: on a PLAIN
// build the freed exception's bytes still read back correctly and `ef339fa`
// prints the right 6. Only `--sanitize=address` sees it. That is the two-oracle
// asymmetry CLAUDE.md records, in this file, measured.
//
// per §27.5, §30.9, §31.2, §30.1 (issue #3503)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
// the helper returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The VALUE printed is the oracle for the rows whose programs still carry an
// unrelated, still-open leak (the async-iterator arm capture, and the guarded
// in-loop cell). A holder that did not survive the suspension does not produce a
// different number in those — it produces no output at all, because the process
// dies at address 0 — so "<did-not-run>" is a distinguishable, failing value.
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// FAIL-WITHOUT rows — the holder must survive the in-arm suspension
// ═══════════════════════════════════════════════════════════════════════════

// The issue's own program, verbatim. `Run(1)` throws, enters the arm, captures
// the binding, suspends inside the arm and reads the binding back afterwards:
// g() + e.Message.Length = 4 + 4 = 8. `Run(0)` never enters the arm and returns
// 1, which pins that giving the holder a home does not disturb the path where no
// capture happens at all.
// per §27.5, §30.9, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           catch_binding_captured_across_in_arm_await_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      if n > 0 then throw new InvalidOperationException(\"boom\"); end if; "
        "      acc := 1; "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        await Task.Delay(1); "
        "        acc := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(1)).ToString()); "
        "    Console.WriteLine((await Run(0)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n1\n");
}

// TWO suspensions inside the same arm, with a read of the capture after each.
// One frame field would be enough for the first; what this row adds is that the
// spill area's holder is carried by EVERY suspension the arm lowers, not just
// the one nearest the capture site. "abcde" is 5: acc = 5, then 5 + 5 + 5 = 15.
// per §27.5, §30.9, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           catch_binding_survives_two_in_arm_awaits_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"abcde\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        await Task.Delay(1); "
        "        acc := acc + g(); "
        "        await Task.Delay(1); "
        "        acc := acc + g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "15\n");
}

// The same arm inside an ASYNC LAMBDA (§30.6). Its frame is the hand-rolled
// fourth state-machine venue, but the repair rides on `asyncHandlerSpillSlots_`
// and `fixupSuspendDominance`, which both venues share — so this row pins that
// the fix is venue-agnostic rather than wired to `emitAsyncMethod`.
// "wxyz" is 4: 4 + 4 = 8.
// per §27.5, §30.6, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           catch_binding_capture_inside_async_lambda_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(): Task<int> => do "
        "      var acc := 0; "
        "      try "
        "        throw new InvalidOperationException(\"wxyz\"); "
        "      catch "
        "        var e: Exception => do "
        "          var g := lambda(): int => e.Message.Length; "
        "          await Task.Delay(1); "
        "          acc := g() + e.Message.Length; "
        "        end "
        "      end try; "
        "      return acc; "
        "    end; "
        "    Console.WriteLine((await f()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n");
}

// The ASYNC ITERATOR venue (§30.7). §29.6 forbids `yield` inside a catch arm, so
// the arm awaits and the values are yielded after the `try`. "pqr" is 3: 3, then
// 3 + 3 = 6. This program carries a PRE-EXISTING 56-byte leak (the caught
// exception object plus two cells) that reproduces identically on `ef339fa` with
// the `await` moved OUT of the arm, and only in this venue — filed as #3507. So
// LeakSanitizer is off here and the generated-code AddressSanitizer stays on.
// per §27.5, §30.7, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           catch_binding_capture_in_async_iterator_spec_27_5_30_7) {
    auto r = runAotCheckedLenient(
        "module M; import System; "
        "class C "
        "  public async static method Gen(): IAsyncEnumerable<int> "
        "    var a := 0; "
        "    var b := 0; "
        "    try "
        "      throw new InvalidOperationException(\"pqr\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        await Task.Delay(1); "
        "        a := g(); "
        "        b := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await for var v in Gen() do "
        "      Console.WriteLine(v.ToString()); "
        "    end for; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n6\n");
}

// LEAK, amplified x500. TWO lambdas capture the same binding: the second reaches
// `boxLocalSlot`'s already-boxed path, which re-guards and — on `ef339fa`, where
// the holder was re-nulled by the resume entry — ALLOCATED A SECOND CELL after
// the suspension. So that tree printed the right 16500 while dropping one 8-byte
// cell per call: 4000 bytes over 500 allocations, invisible to a value oracle and
// invisible to a single-shot LSan row. With the holder carried by the spill area
// the re-guard finds it non-null and no second cell is minted.
// 3 + 30 = 33 per call; 33 x 500 = 16500.
// per §27.5, §28.9, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           catch_binding_two_lambdas_no_second_cell_x500_spec_27_5_28_9) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"abc\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        var h := lambda(): int => e.Message.Length * 10; "
        "        await Task.Delay(1); "
        "        acc := g() + h(); "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + (await Run()); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "16500\n");
}

// The same leak through NESTED try/catch, where the inner arm throws out of
// itself and the outer arm catches. Both arms capture their own binding and both
// suspend, so the program mints two cells per call and dropped both on
// `ef339fa`: 8000 bytes over 1000 allocations. It also pins that the two arms'
// holders get INDEPENDENT spill slots — one shared slot would show up here as a
// wrong number, not a leak. e1 is "ab" (2), then "cdef" (4): 2 + 4 * 10 = 42 per
// call; 42 x 500 = 21000.
// per §27.5, §28.9, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           nested_catch_arms_each_hold_own_cell_x500_spec_27_5_31_2) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      try "
        "        throw new InvalidOperationException(\"ab\"); "
        "      catch "
        "        var e1: Exception => do "
        "          var g1 := lambda(): int => e1.Message.Length; "
        "          await Task.Delay(1); "
        "          acc := acc + g1(); "
        "          throw new InvalidOperationException(\"cdef\"); "
        "        end "
        "      end try; "
        "    catch "
        "      var e2: Exception => do "
        "        var g2 := lambda(): int => e2.Message.Length; "
        "        await Task.Delay(1); "
        "        acc := acc + g2() * 10; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + (await Run()); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "21000\n");
}

// THE SYNCHRONOUS HALF — no `async`, no `await`, no state machine. A `try`/
// `catch` inside a `while`, whose arm captures the binding: guarded boxing
// allocates ONE cell and reuses it, so iteration 2 read iteration 1's exception
// object through the closure — after the arm's own disposal had reclaimed it.
// heap-use-after-free on `ef339fa`, and PRE-EXISTING there: nothing about this
// program involves a suspension. It is in this file because the holder repair
// above would otherwise have converted the async spelling of the same shape from
// a SIGSEGV into exactly this, which is the strictly worse direction.
//
// ⚠ This row is the reason the oracle is `runAotCheckedLenient` and not a plain
// value comparison: on a PLAIN build the freed 40-byte exception still reads back
// correctly and `ef339fa` prints the right 6. The defect is visible only under
// `--sanitize=address`. The remaining 8-byte cell leak is the pre-existing
// guarded-in-loop one (#3498 / #3508), so LeakSanitizer stays off.
// "xy" is 2, three iterations: 6.
// per §27.5, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           sync_reentered_catch_arm_refreshes_cell_spec_27_5_31_2) {
    auto r = runAotCheckedLenient(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 3 do "
        "      try "
        "        throw new InvalidOperationException(\"xy\"); "
        "      catch "
        "        var e: Exception => do "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := acc + g(); "
        "        end "
        "      end try; "
        "      i := i + 1; "
        "    end while; "
        "    return acc; "
        "  end Run; "
        "  public static method Main(): int "
        "    Console.WriteLine(Run().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "6\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLS — green on `ef339fa` as well; they pin that nothing moved
// ═══════════════════════════════════════════════════════════════════════════

// The arm awaits but captures NOTHING. No holder exists, so nothing is
// registered for the spill area and the emitted code must be untouched. This is
// the control that separates "the arm suspends" from "the arm suspends AND
// captures" — the issue's own discriminator table names exactly that pair.
// per §30.9, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           control_awaiting_arm_without_capture_spec_30_9_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        await Task.Delay(1); "
        "        acc := e.Message.Length * 2; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n");
}

// The arm captures but does NOT suspend; the `await` is outside the `try`
// entirely. `lowerTry` registers nothing for a non-awaiting arm, so the holder
// registration below it cannot fire either. Green on both trees.
// per §27.5, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           control_capture_in_arm_with_await_outside_try_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        acc := g() + e.Message.Length; "
        "      end "
        "    end try; "
        "    await Task.Delay(1); "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n");
}

// The ASYNC spelling of the re-entered-arm shape. `ef339fa` gets the VALUE right
// here for an accidental reason — the resume entry re-nulled the holder, so every
// iteration minted a fresh cell — and leaked three of them. With the holder now
// persistent, one cell is minted and refreshed per entry. Both trees print 6, so
// this is a control on the value axis and evidence only in the (still-leaking,
// hence unasserted) allocation count.
// per §27.5, §31.2 (issue #3503)
BEZEL_TEST(catch_binding_capture_across_await_3503,
           control_async_reentered_catch_arm_value_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 3 do "
        "      try "
        "        throw new InvalidOperationException(\"xy\"); "
        "      catch "
        "        var e: Exception => do "
        "          var g := lambda(): int => e.Message.Length; "
        "          await Task.Delay(1); "
        "          acc := acc + g(); "
        "        end "
        "      end try; "
        "      i := i + 1; "
        "    end while; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run()).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "6\n");
}
