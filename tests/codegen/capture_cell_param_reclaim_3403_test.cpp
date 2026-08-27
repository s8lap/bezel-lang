// Codegen — issue #3403 (§27.5 Closures, §28.9 Runtime-managed types, spec
// v0.283): the capture cell of a lambda-captured PARAMETER is never reclaimed
// once the capture site leaves the entry basic block — and, when the capture
// site sits in a nested scope, the cell that IS reclaimed is freed too early
// and the parameter's later reads dereference a nulled holder.
//
// §27.5 mandates the allocation ("Captured variables are heap-allocated in a
// closure object") and §28.9 makes reclaiming it the implementation's own
// obligation ("Function-typed values … carry no disposal obligation" — the
// runtime owes the reclaim instead). Both halves below are that obligation
// going unmet, in opposite directions.
//
// ── WHAT #3403 SAID, AND WHAT IS ACTUALLY BROKEN.
//
// The issue reported a `var` LOCAL captured by a lambda declared after an
// `await` leaking 4 bytes, with the same program clean when the lambda moves
// above the `await`. That program is CLEAN on this tree: #3435 fixed it, by
// establishing that the trigger is a BASIC-BLOCK SPLIT rather than a loop and
// dropping the `inlineN > 0` conjunct from the captured-LOCAL admission. An
// `await` splits blocks, so the issue's shape was the async spelling of #3435.
// The `issue_3403_verbatim_program_…` row below is that program, kept as the
// standing proof of the refutation.
//
// What #3435 did NOT reach is the branch 250 lines above it: the captured-
// PARAMETER admission in `lowerLambda`, which still carried the flat
// `!wasGuardedParam` exclusion. Add a parameter to the issue's own program and
// the leak comes straight back — measured 2000 B / 500 allocations at ×500,
// with the local's cell freed and the parameter's not.
//
// For a parameter the guarded path is not a corner case, which is why this
// mattered more here than it did for a local. `boxLocalSlot` picks fresh-vs-
// guarded by asking whether a StoreInst to the slot sits in the CURRENT basic
// block; a parameter's store is the prologue's, so the fresh path survives only
// while the capture site is still in the entry block. Any block-splitting
// construct above it takes the guarded path — and an ASYNC body takes it
// ALWAYS, because the resume function opens with the state-dispatch `switch`
// and no body statement is ever in the entry block. Hence:
//
//     method Step(v: int): int                    async method Step(v: int)
//       var t := 0;                                 await Task.Delay(1);
//       if v >= 0 then t := 1; end;   /* split */   var f := lambda(x) => x+v;
//       var f := lambda(x) => x+v+t;                return f(3);
//       return f(3);                              /* 4 B leaked, pre-fix */
//     /* 4 B leaked, pre-fix */
//
// The fix is #3435's proof applied to the branch it was not applied to:
// `loopStack_.empty()` is admitted alongside `!wasGuardedParam`. The proof is
// about a BACKWARD EDGE, not about a storage class — with no enclosing loop
// above the capture site, the null-guarded allocation runs at most once per
// invocation and exactly one closure holds the cell.
//
// ── THE SECOND HALF: A PRE-EXISTING SIGSEGV, NOT A LEAK.
//
// Widening a free is only safe if the shapes that must NOT be freed are
// actually excluded, so the exclusions were audited first — and one of them was
// already broken on the pre-fix compiler, independently of anything here:
//
//     method Step(v: int): int
//       var s := 0;
//       do var g := lambda(): int => v; s := g(); end;
//       return s + v;             /* pre-fix: SEGV on address 0 */
//
// §27.5 / §28.9 (#969) skips the scope-end free when the captured entity was
// declared in a SHALLOWER scope than the capture site, because the cell is read
// through after the inner block exits. That guard reads `declScopeDepth_`,
// which holds LOCALS only; its comment noted parameters as an open follow-up
// and pointed at "the disposal guard above", which catches only a
// reads-THROUGH-cell disposal — something a plain `int` parameter does not
// have. So nothing at all covered a captured parameter, `do … end` emits no
// basic block of its own (the parameter therefore took the FRESH path there and
// the free armed), `paramSlots_` had been rebound to the cell holder, and
// `return s + v` dereferenced the holder the inner scope had just nulled:
// "AddressSanitizer: SEGV on unknown address 0x000000000000", measured.
//
// The fix marks the entry `isParam` and applies #969's rule to it. A parameter
// is bound in the body's OUTERMOST scope by construction — every body lowering
// clears `disposeStack_` and pushes exactly one scope before the first
// statement — so "declared shallower than the capture site" is exactly
// `disposeStack_.size() > 1`, with no depth to record at `paramSlots_`'s many
// population sites. The cell then leaks until exit: the same sound trade-off
// #969 took for locals, and the correct direction of danger.
//
// ── DIRECTION OF DANGER. This admits a FREE: under-admitting leaks,
// over-admitting is a use-after-free. The three guard rows assert shapes whose
// cell must NOT be reclaimed at the capture site's scope end; each leaks
// exactly 80 B / 20 allocations both before and after, and that EQUALITY is the
// assertion that nothing extra was admitted.
//
// ONE of the three is PROVEN FALLIBLE, against the single conjunct it names, on
// a scratch compiler built with that conjunct deleted:
//
//   guard_param_captured_in_nested_do_block_…   `ce.isParam &&
//                                                disposeStack_.size() > 1`
//                                               removed -> "ERROR:
//                                               AddressSanitizer: SEGV on
//                                               unknown address
//                                               0x000000000000"
//
// Every other row in this file stayed green on that compiler. The report is a
// SEGV rather than a heap-use-after-free because the disposal NULLS the holder
// after freeing, so the later read dereferences null rather than freed memory —
// which is also why this defect was silently fatal instead of merely dirty.
//
// The other two are recorded as what they measurably are — leak-count and value
// PARITY rows, not proven-fallible guards.
//   * `guard_param_captured_inside_a_loop_…` did NOT turn red with BOTH new
//     conjuncts deleted: it went CLEAN. The shape is safe by accident, because
//     the scope-end free nulls the holder, so the next iteration's null-guarded
//     boxing allocates a FRESH cell instead of resharing the freed one. It stays
//     excluded anyway — the accident is not a proof, and closures that escape
//     an iteration would have no such luck.
//   * `guard_param_captured_by_escaping_lambda_…` never consults the widened
//     admission at all (`collectCellFreeEligible` excludes the parameter from
//     `cellFreeEligible_` outright), so no conjunct here can turn it red.
//
// The two new conjuncts OVERLAP for a parameter: a loop body is a nested scope,
// so the depth guard alone already excludes the in-loop shape, and deleting
// `loopStack_.empty()` ALONE turns nothing red — measured, not assumed. It is
// kept as the honest mirror of #3435's captured-LOCAL admission and so the
// widening does not rest on the depth invariant alone, and this comment says
// that rather than claiming a guard that is not independently fallible.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   program                                        pre-fix          post-fix
//   async param capture after `await`, ×500        2000 B / 500     clean
//   sync param capture behind an `if`, ×500        2000 B / 500     clean
//   `string` param capture behind an `if`, ×500    4000 B / 500     clean
//   async param + local, issue's shape, ×500       2000 B / 500     clean
//   control: no split above the capture, ×500      clean            clean
//   #3403's verbatim program (a LOCAL)             clean            clean
//   guard: param captured in a nested `do … end`   SEGV @ 0         80 B / 20
//   guard: param captured inside a loop            80 B / 20        80 B / 20
//   guard: param captured by an escaping lambda    80 B / 20        80 B / 20
//
// per §27.5, §28.9 (issue #3403; refutes its stated shape, fixes the adjacent
// captured-PARAMETER one and the #969 parameter hole)

#include "support/aot_support.hpp"   // runAot / runAotChecked — ASan/LSan AOT
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report turns into ran=false and the sentinel below.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the guard rows, whose shapes leak BY DESIGN (a cell no
// mechanism may reclaim without a use-after-free). The oracle there is the
// exact VALUE plus completion: an over-eager free shows up as a wrong number,
// a SIGSEGV, or an ASan abort under the `--full` gate — the pre-fix nested-`do`
// row failed on exactly that, and the value here is what it could not produce.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The captured-PARAMETER cell is reclaimed once the capture site is guarded
// ═══════════════════════════════════════════════════════════════════════════

// ── ASYNC: the resume function's state-dispatch `switch` means no body
// statement is ever in the entry block, so a captured parameter ALWAYS took the
// guarded path and its cell was ALWAYS dropped. Amplified ×500, because a
// one-shot 4-byte LSan row can pass on both sides of the fix while the single
// allocation is still stack-reachable at exit — measured pre-fix at
// 2000 B / 500 allocations, which no reachability accident explains.
// Printed total: sum over j of (3 + j) = 1500 + 124750 = 126250.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           async_param_capture_after_await_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method Step(v: int): Task<int> "
        "    await Task.Delay(1); "
        "    var f := lambda(x: int): int => x + v; "
        "    return f(3); "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + await C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "126250\n");
}

// ── SYNC, NO AWAIT ANYWHERE. The same leak with an `if` as the only splitter,
// which is what makes the root cause falsifiable rather than plausible: had the
// mechanism been about `await`, or about async frames, this program would have
// been clean pre-fix. It leaked 2000 B / 500 identically.
// Printed total: sum over j of (3 + j + 1) = 2000 + 124750 = 126750.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           sync_param_capture_behind_if_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var t := 0; "
        "    if v >= 0 then t := 1; end; "
        "    var f := lambda(x: int): int => x + v + t; "
        "    return f(3); "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "126750\n");
}

// ── A RUNTIME-MANAGED (§28.9 `string`) PARAMETER. Its cell is 8 bytes rather
// than 4, and the capture path around it is busier: §28.9 makes every capture
// of a `string` PARAMETER take a `bz_string_retain` (the parameter is an
// unmodified borrow — there is no suppressed scope-end release for the first
// env to inherit) which the env's destructor releases. Freeing the cell must
// not disturb that pairing; the exact printed value is the assertion that it
// does not, alongside the leak check. Pre-fix: 4000 B / 500 allocations.
// Printed total: 500 × (4 + 1) = 2500.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           string_param_capture_cell_freed_amplified_500_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Step(s: string): int "
        "    var t := 0; "
        "    if s.Length > 0 then t := 1; end; "
        "    var f := lambda(): int => s.Length + t; "
        "    return f(); "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(\"abcd\"); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2500\n");
}

// ── THE ISSUE'S OWN SHAPE, WITH A PARAMETER ADDED. A local declared before the
// `await` and a parameter, both captured by one lambda declared after it. The
// local's cell is freed (that is #3435's fix); the parameter's was not, which
// is why the pre-fix report was 2000 B / 500 and not 4000 B / 500 — one cell
// per call, not two. Printed total: sum over j of (3 + 7 + j) = 5000 + 124750.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           async_param_and_local_captured_after_await_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method Step(v: int): Task<int> "
        "    var k: int := 7; "
        "    await Task.Delay(1); "
        "    var f := lambda(x: int): int => x + k + v; "
        "    return f(3); "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + await C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "129750\n");
}

// ── CONTROL: no splitter above the capture site, so the parameter takes
// `boxLocalSlot`'s FRESH path and the free was already armed pre-fix. This row
// passes on BOTH compilers deliberately — it is what makes the four rows above
// evidence about the GUARDED path specifically rather than about captured
// parameters in general. Printed total: sum over j of (3 + j) = 126250.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           control_unsplit_param_capture_was_already_clean_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var f := lambda(x: int): int => x + v; "
        "    return f(3); "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<500 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "126250\n");
}

// ── #3403'S VERBATIM PROGRAM — THE REFUTATION, PINNED. Copied character for
// character from the issue body (a `var` LOCAL captured by a lambda declared
// after an `await`, reported as 4 bytes leaked). It is clean on this tree and
// was clean before this commit too: #3435 fixed it. The row exists so the
// refutation is a standing, mechanically checked claim rather than a sentence
// in a commit message — if this shape ever leaks again, this fails.
// per §27.5, §30.1 (issue #3403, as filed)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           issue_3403_verbatim_local_after_await_is_clean_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var k: int := 7; "
        "    await Task.Delay(20); "
        "    var f := lambda(x: int) => x + k; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ═══════════════════════════════════════════════════════════════════════════
// OVER-RELEASE GUARDS — shapes whose cell must NOT be freed at the capture
// site's scope end. Each leaks 80 B / 20 allocations before AND after; the
// equality is the assertion that the widening admitted nothing extra.
// ═══════════════════════════════════════════════════════════════════════════

// ── PROVEN FALLIBLE. The capture site is inside a nested `do … end`, and the
// parameter is read again after the block. Pre-fix this was a SIGSEGV on
// address 0 — the free armed (a `do` block emits no basic block of its own, so
// the parameter took the FRESH path and `!wasGuardedParam` admitted it), nulled
// the holder at the inner scope's end, and `return s + v` dereferenced it.
// `ce.isParam && disposeStack_.size() > 1` now declines the free; the cell
// leaks instead, which is the sound direction. Deleting THAT conjunct alone on
// a scratch compiler turns this row into "ERROR: AddressSanitizer: SEGV on
// unknown address 0x000000000000" and leaves every other row in the file green.
// Printed total: sum over j of (j + j) = 2 × 190 = 380.
// per §27.5, §28.9 (issue #3403, #969)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           guard_param_captured_in_nested_do_block_is_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var s := 0; "
        "    do "
        "      var g := lambda(): int => v; "
        "      s := g(); "
        "    end; "
        "    return s + v; "
        "  end Step; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<20 do acc := acc + C.Step(j); end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "380\n");
}

// ── PARITY, not proven fallible — and the reason that distinction is worth
// paying for. One syntactic lambda node inside a loop makes N closures over ONE
// guarded cell, so a free registered at the loop body's scope end would run
// once per iteration, which is what `loopStack_.empty()` is there to prevent.
// It could not be turned red: with BOTH new conjuncts deleted this row went
// CLEAN, not double-free, because the scope-end free NULLS the holder and the
// next iteration's null-guarded boxing therefore allocates a fresh cell rather
// than resharing the freed one. The shape is safe by accident; the exclusion
// stays, because the accident is not a proof and an escaping per-iteration
// closure would not enjoy it.
//
// ⚠ What this row actually asserts is VALUE parity across the fix, not leak
// parity: it runs through `plainOut` (`runAot`), whose oracle is the value and
// completion only — it never observes a leak count. The leak-count equality was
// measured out of band and is recorded in the commit body; do not read this row
// as pinning it. Printed total: sum over j of (3j + 3) = 630.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           guard_param_captured_inside_a_loop_is_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Step(v: int): int "
        "    var acc := 0; "
        "    for var i in 0..<3 do "
        "      var g := lambda(): int => v + i; "
        "      acc := acc + g(); "
        "    end; "
        "    return acc; "
        "  end Step; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var j in 0..<20 do t := t + C.Step(j); end; "
        "    Console.WriteLine(t.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "630\n");
}

// ── PARITY, not proven fallible. The lambda ESCAPES by return, so its env holds
// a raw cell pointer that outlives the frame and `collectCellFreeEligible`
// excludes the parameter from `cellFreeEligible_` outright — the widened
// admission is never consulted. #694's envdtor route declines this shape too
// (a conservative leak that predates this commit and is left alone). The row is
// here as a leak-count and value equality across the fix: had the widening
// leaked into the escaping path, the returned closure would read a freed cell.
// Printed total: sum over j of (1 + j + 1) = 190 + 40 = 230.
// per §27.5, §28.9 (issue #3403)
BEZEL_TEST(capture_cell_param_reclaim_3403,
           guard_param_captured_by_escaping_lambda_is_not_freed_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; "
        "class C "
        "  public static method Make(v: int): (int) -> int "
        "    var t := 0; "
        "    if v >= 0 then t := 1; end; "
        "    return lambda(x: int): int => x + v + t; "
        "  end Make; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<20 do "
        "      var f := C.Make(j); "
        "      acc := acc + f(1); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "230\n");
}
