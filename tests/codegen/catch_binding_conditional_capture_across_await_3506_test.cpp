// Codegen — issue #3506 (§27.5 Closures, §30.9 the async fault boundary,
// §31.2 `catch` arms, spec v0.286): a lambda captures a CATCH-ARM BINDING
// inside an awaiting arm BEHIND A CONDITIONAL, the capture never runs on the
// branch where the condition is false, and the read after the `await`
// dereferences a NULL capture-cell holder (SIGSEGV at address 0).
//
//     async method Run(n: int): Task<int>
//       try   … throw …
//       catch var e: Exception => do
//         if n mod 2 = 0 then
//           var g := lambda(): int => e.Message.Length;
//           acc := g();
//         end if;
//         await Task.Delay(1);
//         acc := acc + e.Message.Length;      /* SIGSEGV @ 0 on the odd branch */
//       end
//       end try;
//
// MECHANISM. #3503 closed the same shape WITHOUT the conditional: it gave the
// holder the handler SPILL AREA on the existing `asyncHandlerSpillSlots_`
// authority, so the holder persists across the suspension. With the
// conditional, the FIRST pass (n even) populates the cell as before and the
// post-await read goes through the cell — fine. The SECOND pass (n odd)
// never reached the capture site, so the cell was never allocated, the
// holder is still the calloc-zero spill-area slot, and the after-await read
// loads null and dereferences it. The unconditional shape (#3503) does not
// exercise this branch because the guard's alloc branch already partitioned
// "holder non-null → read folds to cell". The conditional shape exposes it.
//
// #3484's fall-back deliberately DECLINED this shape because the fall-back
// address — the binding's OWN alloca — was the very slot the demotion ERASED.
// Relaxing the decline needs BOTH halves:
//
//   * `boxedStorage` now records `syncSurvivesSuspend = true` when the sync
//     slot is named in `asyncHandlerSpillSlots_` at read-time (BEFORE
//     `fixupSuspendDominance` clears the vector). That is the read-time
//     witness that the storage round-trips through the spill area.
//
//   * `fixupCapturedCellHolderReads` now admits a `GetElementPtrInst` in the
//     read's own function as the fall-back value. That is the post-RAUW form
//     of the original alloca — a structured GEP computed in `entry.spillcont`
//     which dominates every state block, into a heap-persistent spill area.
//     `WeakTrackingVH` still nulls the handle on true deletion, so the
//     raw-pointer-recycle hazard `fix #3484 (follow-up)`'s D2 guarded against
//     is not reintroduced.
//
// Together they close the conditional shape and pin it. The fix widens no
// free and shortens no lifetime: a cell never allocated was already never
// freed (`free(null)` is a no-op).
//
// FAIL-WITHOUT / PASS-WITH, measured one program at a time. Compared to the
// unconditional sibling pinned in
// `catch_binding_capture_across_await_3503_test.cpp`:
//
//   row                                              de879ab  this
//   conditional capture + in-arm await, four runs    SEGV @ 0  8 4 8 4
//   two in-arm awaits with conditional capture       SEGV @ 0  15 10
//   conditional capture inside async lambda         SEGV @ 0  8 3 8 3
//   conditional capture, x500 amp                   SEGV @ 0  3000
//   nested try, inner conditional + outer uncond    SEGV @ 0  21500
//   control: unconditional in arm                   8         8
//   control: conditional + await OUTSIDE            4 0 4 0   4 0 4 0
//   control: sync, no await, conditional            8 4 8 4   8 4 8 4
//
// All five FAIL-WITHOUT rows above crash on the de879ab build (no fix in this
// commit). The three CONTROLS stay green on both de879ab and the fix; they
// pin that nothing UNRELATED moved when the conditional admit was added.
//
// ⚠ The amplified rows need `--sanitize=address` specifically. On a PLAIN
// build the first SEGV dominates the test and the LSan report never gets a
// chance to fire — the value is right (3000 / 21500), the SEGV is the
// failure. Recorded here so the row uses `runAotChecked` not `runAot`, and so
// the test cannot accidentally pass on a partial fix that swallowed only the
// crash.
//
// per §27.5, §30.9, §31.2 (issue #3506)

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

// Wrapper: a process that died at address 0 returns no stdout. The runAot
// helper maps that to `ran=false` (compile passes, link passes, exec dies).
// The VALUE-side rows below assert on the printed text; the CRASH rows
// assert that the process DID run (no `<did-not-run>` substitution) and
// completed under the helper's exit-status contract — captured separately
// for crash-pinning shape.
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// FAIL-WITHOUT rows — the conditional capture's holder must fall back to the
// spill-area slot, not read null
// ═══════════════════════════════════════════════════════════════════════════

// The issue's own program, verbatim. n even: 4+4 = 8. n odd: 0+4 = 4.
// Both branches must print the EXPECTED value, not die.
// per §27.5, §30.9, §31.2 (issue #3506)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           conditional_capture_in_arm_await_four_runs_spec_27_5_30_9) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        if n mod 2 = 0 then "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := g(); "
        "        end if; "
        "        await Task.Delay(1); "
        "        acc := acc + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 4 do "
        "      Console.WriteLine((await Run(i)).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n4\n8\n4\n");
}

// TWO suspensions inside the same arm, conditional on `n`. Each post-await
// read of the binding must independently use the spill-area fall-back when
// the conditional capture never ran (n odd). "abcde" is 5. n=1 is the branch
// that exercised the bug: lambda's lambda-creating line is gated, so the
// capture site never ran, so each post-await read loads the calloc-zero
// holder without the admit — pre-fix that is SEGV at the first await.
// n=1 expected: 0 + 5 + 5 = 10. n=0 expected: 5 + 5 + 5 = 15.
// per §27.5, §30.9, §31.2 (issue #3506)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           conditional_capture_two_in_arm_awaits_spec_27_5_30_9) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"abcde\"); "
        "    catch "
        "      var e: Exception => do "
        "        if n mod 2 = 0 then "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := g(); "
        "        end if; "
        "        await Task.Delay(1); "
        "        acc := acc + e.Message.Length; "
        "        await Task.Delay(1); "
        "        acc := acc + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    Console.WriteLine((await Run(0)).ToString()); "
        "    Console.WriteLine((await Run(1)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "15\n10\n");
}

// Same shape inside an async LAMBDA (§30.6), conditional on a parameter that
// is even and another that is odd. The repair rides on `asyncHandlerSpillSlots_`
// and `fixupSuspendDominance`, which are shared across venues — so this row
// pins that the conditional admit is venue-agnostic rather than wired to
// `emitAsyncMethod` specifically. label even: "wxyz" (4), g() runs ⇒ acc=4,
// await, acc+=4 ⇒ 8. label odd: "wxy" (3), no lambda branch runs ⇒ acc=0,
// await, acc+=3 ⇒ 3.
// per §27.5, §30.6, §31.2 (issue #3506)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           conditional_capture_in_async_lambda_spec_27_5_30_6) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(label: int): Task<int> => do "
        "      var acc := 0; "
        "      var msg: string; "
        "      if label mod 2 = 0 then "
        "        msg := \"wxyz\"; "
        "      else "
        "        msg := \"wxy\"; "
        "      end if; "
        "      try "
        "        throw new InvalidOperationException(msg); "
        "      catch "
        "        var e: Exception => do "
        "          if label mod 2 = 0 then "
        "            var g := lambda(): int => e.Message.Length; "
        "            acc := g(); "
        "          end if; "
        "          await Task.Delay(1); "
        "          acc := acc + e.Message.Length; "
        "        end "
        "      end try; "
        "      return acc; "
        "    end; "
        "    Console.WriteLine((await f(4)).ToString()); "
        "    Console.WriteLine((await f(3)).ToString()); "
        "    Console.WriteLine((await f(2)).ToString()); "
        "    Console.WriteLine((await f(1)).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n3\n8\n3\n");
}

// LEAK + CRASH, amplified x500. The issue's own shape, repeatedly: each
// call's conditional branch leaves the holder null when n is odd, and the
// post-await read of the binding MUST read through the fall-back rather
// than dereferencing the calloc-zero holder. Pre-fix: SIGSEGV @ 0 on the
// first n-odd call (i=1). Post-fix: no crash, correct values. Per call:
// n=even ⇒ 8, n=odd ⇒ 4; over 500 iterations of i=0..499 the sum is
// 250 even * 8 + 250 odd * 4 = 3000. The amplification is for CRASH
// detection reliability — 250 odd iterations is many chances to surface a
// single SIGSEGV rather than relying on the harness noticing one in four
// runs of a non-amplified program. Leak-wise, on n-odd no cell is allocated
// (so no leak there); on n-even one cell is allocated and freed by the
// arm-scope end-of-storage reclaim.
// per §27.5, §28.9, §31.2 (issue #3506)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           conditional_capture_x500_no_crash_spec_27_5_28_9) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        if n mod 2 = 0 then "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := g(); "
        "        end if; "
        "        await Task.Delay(1); "
        "        acc := acc + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 500 do "
        "      acc := acc + (await Run(i)); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3000\n");
}

// NESTED try/catch with a CONDITIONAL capture inside the inner (re-throwing)
// arm AND an unconditional read of the binding after an in-arm `await` — the
// conditional admit matters because the binding's holder is null on the
// no-capture branch and the post-await read goes through the spill-area GEP.
// Inner arm: e1 is "ab" (2), conditionally captures and read-then-rethrows.
// Outer arm: e2 is "cdef" (4), unconditional capture and a single await.
// Per iteration of `n`:
//   n even: g1() runs ⇒ acc = 2; await; acc += 2 ⇒ 4; throw cdef.
//           outer: e2 = "cdef" (4); acc += g2()*10 ⇒ 44.   Run(n even) = 44.
//   n odd:  no lambda; await; acc += 2 ⇒ 2; throw cdef.
//           outer: acc += g2()*10 ⇒ 42.                     Run(n odd) = 42.
// x500 of i=0..499 ⇒ 250*44 + 250*42 = 11000 + 10500 = 21500.
// Pre-fix: SIGSEGV on the first odd iteration (i=1) — the inner arm's
// post-await `e1.Message.Length` read loads the calloc-zero holder. With the
// fix, the fall-back to the spill-area GEP reads the binding correctly, and
// the rethrow lands in the outer arm which is independent storage.
// per §27.5, §28.9, §31.2 (issue #3506)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           nested_catch_arms_conditional_x500_spec_27_5_31_2) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      try "
        "        throw new InvalidOperationException(\"ab\"); "
        "      catch "
        "        var e1: Exception => do "
        "          if n mod 2 = 0 then "
        "            var g1 := lambda(): int => e1.Message.Length; "
        "            acc := acc + g1(); "
        "          end if; "
        "          await Task.Delay(1); "
        "          acc := acc + e1.Message.Length; "
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
        "      acc := acc + (await Run(i)); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "21500\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROLS — green on `ef339fa + #3503` as well; they pin that nothing
// UNRELATED moved when the conditional admit was added
// ═══════════════════════════════════════════════════════════════════════════

// UNCONDITIONAL capture in an awaiting arm. #3503 already closes this shape;
// the conditional admit above MUST NOT regress it. "wxyz" is 4: 4 + 4 = 8.
// per §27.5, §30.9, §31.2 (issue #3503 — regression pin on the #3506 path)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           control_unconditional_capture_spec_27_5_30_9) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"wxyz\"); "
        "    catch "
        "      var e: Exception => do "
        "        var g := lambda(): int => e.Message.Length; "
        "        acc := g(); "
        "        await Task.Delay(1); "
        "        acc := acc + e.Message.Length; "
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

// Conditional capture in the arm, `await` OUTSIDE the try. `lowerTry`
// registers nothing for a non-awaiting arm, so the conditional admit cannot
// fire here. Even-branch and odd-branch outputs: even ⇒ acc := g() = 4;
// odd ⇒ no lambda branch runs ⇒ acc = 0. Then await, then return acc
// unchanged. Pre- and post-fix both print `4 0 4 0`.
// per §27.5, §31.2 (issue #3506 control)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           control_conditional_capture_await_outside_try_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static async method Run(n: int): Task<int> "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        if n mod 2 = 0 then "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := g(); "
        "        end if; "
        "      end "
        "    end try; "
        "    await Task.Delay(1); "
        "    return acc; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 4 do "
        "      Console.WriteLine((await Run(i)).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "4\n0\n4\n0\n");
}

// SYNCHRONOUS method (no async, no await). #3484's read-side fall-back
// already covers this shape without the state-machine plumbing the fix
// touches. Pre- and post-fix both print `8 4 8 4`.
// per §27.5, §31.2 (issue #3506 control — sync is the pre-existing PATH)
BEZEL_TEST(catch_binding_conditional_capture_across_await_3506,
           control_sync_no_await_conditional_spec_27_5_31_2) {
    BEZEL_EXPECT_EQ(out(
        "module M; import System; "
        "class C "
        "  public static method Run(n: int): int "
        "    var acc := 0; "
        "    try "
        "      throw new InvalidOperationException(\"boom\"); "
        "    catch "
        "      var e: Exception => do "
        "        if n mod 2 = 0 then "
        "          var g := lambda(): int => e.Message.Length; "
        "          acc := g(); "
        "        end if; "
        "        acc := acc + e.Message.Length; "
        "      end "
        "    end try; "
        "    return acc; "
        "  end Run; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 4 do "
        "      Console.WriteLine(Run(i).ToString()); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "8\n4\n8\n4\n");
}
