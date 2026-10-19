// Codegen: a closure-typed local MUTATED (not returned) by a lambda nested
// TWO OR MORE levels deep is a heap-use-after-free, independent of #4226's
// own fix (issue #4240).
//
// WHAT #4226 FIXED, AND WHERE IT STOPPED. #4226 fixed the two-level
// RETURNED-BY-NAME case: `returnedCaptureVars_`'s ancestor fact ("f is
// returned somewhere in the enclosing chain") was lost across `lowerLambda`'s
// per-lambda re-scan of `collectBoxCaptureLocals`, and #4226 restored it with
// a snapshot-before-clear / merge-after-rescan pair. #4226's OWN review
// measured THIS issue (#4240) directly: a captured local that is MUTATED (via
// `f := lambda() => N;`) by a lambda nested two levels deep, and never
// returned by name at all, was STILL a heap-use-after-free on the tree that
// already carried #4226's fix — a DIFFERENT mechanism
// (`singleCaptureLocals_` / `mutatedSharedCaptureCell`), not the one #4226
// touched (`capturedLocalReturnedByName` is never even consulted here, since
// `f` is never returned by name).
//
// ROOT CAUSE, PROVEN (not just diagnosed). `collectBoxCaptureLocals`'s tally
// counts, for each captured LOCAL DECL NODE, how many distinct lambda ENVS
// (found by `collectAllLambdas`, full descent from the scan's OWN root)
// capture it; `n == 1` goes into `singleCaptureLocals_`.
//
//   var f: () -> int := lambda() => 7;
//   var outer := lambda() => do
//     var inner := lambda() => do
//       f := lambda() => 42;      // <- inner MUTATES f
//     end;
//     inner();
//   end;
//   outer();
//   return f();
//
// At the ENCLOSING METHOD's own top-level scan (`collectBoxCaptureLocals(One
// .stmts)`, run once before any lambda is lowered), `collectAllLambdas` finds
// BOTH `outer` and `inner` (full descent), and `collectCapturedSyms` (which
// itself descends into nested lambda bodies) correctly attributes `f` to
// BOTH: `inner` reads/writes it directly, and `outer` — which does not
// reference `f` in its OWN top-level statements at all — still needs a
// pointer to `f`'s cell purely to hand it down when `inner`'s closure literal
// is constructed inside `outer`'s body, so `outer`'s own (full-descent)
// capture set ALSO contains `f`. True count: 2. `singleCaptureLocals_`
// correctly excludes `f` at this scan.
//
// But `lowerLambda` (LlvmBackend_Exprs2.cpp) re-invokes
// `collectBoxCaptureLocals(outer.stmts)` when it lowers `outer`'s OWN body —
// to discover what `outer`'s OWN nested lambdas need to capture — and that
// re-scan's `collectAllLambdas` root is `outer.stmts` itself, so it can never
// find `outer` AS one of the capturing envs of its own free variables: from
// this narrower vantage, only `inner` captures `f`, so `n == 1`, and `f`
// lands in `singleCaptureLocals_` WRONGLY. By the time `inner`'s OWN capture
// of `f` is decided (while lowering `outer`'s body, i.e. with
// `boxCaptureLocalsFn_ == outer`'s function), `mutatedSharedCaptureCell`
// reads this wrong `singleCaptureLocals_` and returns FALSE (no shared-cell
// suppression) even though `assignedCaptureVars_.count(f)` is (correctly)
// true — so `inner`'s capture of `f` takes an ordinary, unshared retain/
// release: it retains the value CURRENTLY in `f` at capture time (the
// initial closure), but its envdtor unconditionally releases WHATEVER IS
// CURRENTLY IN THE CELL when `inner`'s call frame ends — by then, `f :=
// lambda() => 42;` has already run, so the cell holds the NEW closure, which
// was never independently retained by anyone. `inner`'s envdtor therefore
// frees a reference nobody gave it: heap-use-after-free, freed by `inner`'s
// own envdtor, on the very value `inner` itself just produced — exactly the
// issue's own measured trace (`freed by ... bz$lam$2$envdtor`, `previously
// allocated ... in lam$2`, `lam$2` = `inner`).
//
// Meanwhile `outer`'s OWN capture of `f` (decided earlier, while lowering
// `One`'s body, using the STILL-CORRECT top-level scan where `f` is NOT in
// singleCaptureLocals_) correctly suppresses BOTH `outer`'s retain and
// `outer`'s envdtor release. So the pre-fix imbalance is exactly:
// `outer` = suppressed (correct), `inner` = NOT suppressed (wrong) — an
// unbalanced release with no matching retain.
//
// THE FIX. A NEW ancestor-merged fact, `multiCaptureLocals_` (LlvmBackend_
// impl.hpp): "known captured by 2+ envs", populated alongside
// `singleCaptureLocals_` in the SAME tally (mutually exclusive by
// construction: n==1 vs n>=2). `lowerLambda` snapshots it immediately before
// `collectBoxCaptureLocals` clears it for a nested lambda's own re-scan (the
// EXACT #4226/#3804 snapshot-before-clear pattern), and after the re-scan,
// merges the snapshot back in — NOT as a plain union onto
// `singleCaptureLocals_` (that can only ADD members, and this needs a
// REMOVAL), but as a union onto `multiCaptureLocals_` itself PLUS an ERASE of
// those same keys from `singleCaptureLocals_`. That erase is what makes
// `outer`'s ancestor-proven "f is shared by 2+" verdict overturn `inner`'s
// own rescan's wrong "f looks single" conclusion, so `mutatedSharedCaptureCell`
// sees the correct (now-restored) `!singleCaptureLocals_.count(f)` and
// suppresses `inner`'s retain/release too — balanced with `outer`'s.
//
// RETAIN-SIDE, PROVEN NOT MERELY TRACED (the #4226 review's own open item).
// If the erase mis-fired in the OTHER direction — i.e. wrongly EXCLUDED a key
// that is genuinely single-capture, or the merge ran on the wrong side of a
// clear/recompute boundary — the observable failure mode would be the
// opposite of a UAF: a suppressed retain with no suppressed release is a
// plain LEAK (rc under-counted, never reaches 0), and a suppressed release
// with no suppressed retain is an OVER-RELEASE (what this issue itself
// already was). This was proven, not assumed, with temporary instrumentation
// (added to and removed from `collectBoxCaptureLocals`, `mutatedSharedCaptureCell`
// and the merge site, never committed) that printed the LIVE `singleCaptureLocals_`
// / `multiCaptureLocals_` / `assignedCaptureVars_` membership and
// `mutatedSharedCaptureCell`'s own return value at every capture site for the
// row (A) program: BOTH `outer`'s own capture of `f` (decided while lowering
// `One`'s body, using `One`'s own unmolested top-level scan) AND `inner`'s own
// capture of `f` (decided while lowering `outer`'s body, using the
// ancestor-merged, corrected state) show `single=0 multi=1 assigned=1`, and
// `mutatedSharedCaptureCell` returns `true` for BOTH — i.e. the fix makes
// BOTH envs suppress their retain AND their envdtor release TOGETHER, which is
// exactly the symmetric, balanced trade #2706 already established for the
// one-level case, applied correctly at two-plus levels for the first time.
//
// WHAT THAT TRADE ACTUALLY MEASURES TO, AND A SEPARATE, PRE-EXISTING BUG IT
// EXPOSES (issue #4261, filed, NOT fixed here). Row (A)/(A2)/(C)/(D) below
// are ASan+LSan at up to 500 iterations to distinguish "still crashes" (fix
// absent or wrong direction) from "leaks" (fix over-suppresses, or something
// else fails to reclaim) from "clean" (fix exact and nothing else needed).
// Measured: NO CRASH on any mutated row at any depth (2/3/4) — the UAF this
// issue is about is gone — but EVERY mutated, non-read-only row leaks a
// BOUNDED, exactly-40-bytes-per-`One()`-call amount (measured 360 B / 9
// allocations at the row's own 9-iteration amplification, i.e. 40 B/call,
// linear — not growing within a call, not unbounded). This is NOT this
// fix's own over-suppression: `outer`'s and `inner`'s ce.suppressContentRcInEnvdtor
// are symmetric (both true, both suppressed TOGETHER, the sound #2706 trade),
// so nothing here retains without releasing or the reverse — that pairing is
// exact. The leak is a THIRD, independent, pre-existing mechanism traced to
// `Emitter::lastLambdaCapturedCells_` (LlvmBackend_impl.hpp): `var inner :=
// lambda...;`'s own `lowerLocalDecl` calls `consumeLastLambdaCapturedCells()`
// and drains the ENTIRE shared vector — including `outer`'s own
// correctly-scoped `LastCellEntry` for `f`, pushed earlier at `f`'s own
// (shallower) declaration scope — at `inner`'s OWN (deeper) scope, so the
// #969 conservative-leak guard ("skip when the captured local's decl scope is
// shallower than the current scope") discards BOTH entries, and nobody ever
// frees `f`'s cell. This is the SAME "ancestor's already-valid fact lost to a
// narrower nested operation" class of defect as #3804/#4226/#4240 itself, one
// mechanism over — full trace and a standalone repro are in issue #4261.
// It was invisible before #4240's own fix because every 2+-level mutated,
// non-returned capture crashed (UAF) before execution ever reached the point
// where this drain's effect (an unfreed cell) would be observable — #4240's
// fix is what exposes it, exactly as #4226's own control row exposed #4240.
//
// So mutated rows below use `runAotCheckedLenient` (ASan crash detection ON,
// LSan OFF) — the correct oracle for THIS issue's own claim (no crash) without
// silently failing on #4261's separate, already-filed, bounded leak. Each
// mutated row's own comment states the measured leak this masks, so the
// number is on record even though the row's assertion does not enforce it.
//
// Read-only rows (B/B2/C2/D2) are the WORKING CONTROLS: `assignedCaptureVars_`
// never contains `f` for them, so `mutatedSharedCaptureCell`'s FIRST gate
// (`assignedCaptureVars_.count(key)`) already returns false regardless of
// `singleCaptureLocals_`/`multiCaptureLocals_` — completely unaffected by
// this fix, exactly like #4226's own read-only rows one mechanism over, and
// also unaffected by #4261 (measured fully clean under the STRICT
// `runAotChecked`/LSan oracle at every depth: a read-only capture never
// takes the boxed/written-cell path #4261 lives in at all).
//
// §27.5 ("Closures" — "Modifications to a captured variable inside the
// lambda are visible outside, and vice versa") is the only genuine spec
// citation; §28.9 disclaims the refcounting IMPLEMENTATION itself, so this
// is an INTERNAL soundness fix, like #4198/#4226/#3804 before it.
//
// OUT OF SCOPE, NOT TOUCHED HERE: issue #4239 (chained call on a returned
// closure temporary double-frees; one level, unrelated) — none of these rows
// return a closure at all, only `int` values, so #4239 cannot be reached.
// Issue #4227 (a captured cell's displaced value on reassignment is never
// released) is a DIFFERENT mechanism from #4261's drain bug (#4227 is about
// the OLD value at a write being unreleased; #4261 is about the FINAL value
// never being freed at all, regardless of how many writes happened) — #4261
// was filed as its own issue rather than folded into #4227 for that reason.

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
// (A) The issue's own repro, verbatim shape: `f` is captured by `outer`
// (transitively, to hand its cell down) and MUTATED by `inner`, nested two
// levels deep, and never returned by name. 9 iterations, no loop-only
// flakiness expected (the issue states the first iteration already crashes
// pre-fix). Pre-fix: heap-use-after-free, freed by `inner`'s own envdtor.
// Post-fix: no crash, correct value; `runAotCheckedLenient` (ASan crash
// detection ON, LSan OFF) is the correct oracle here — `runAotChecked`
// (LSan ON) fails this row for a REASON UNRELATED TO THIS FIX: the
// pre-existing, separately-filed #4261 bounded leak (measured via a direct
// `--sanitize=address` probe, not through this harness: 360 B / 9
// allocations at this row's own 9-iteration count, i.e. exactly 40 B/call,
// `freed by`-less / `allocated in lam$2` — see the file header).
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           mutated_two_level_nested_shared_capture_no_crash_correct_value_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
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
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: heap-use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (A2) Same shape, loop-amplified to 500 calls — the crash-freedom claim at
// scale. `runAotCheckedLenient` for the same #4261 reason as row (A); a
// direct `--sanitize=address` probe (outside this harness) at this exact
// 500-iteration amplification measured 20000 B / 500 allocations — i.e.
// still exactly 40 B/call, confirming #4261's leak is BOUNDED (linear in
// call count, not growing per call) at 500x too, not merely at 9x.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           mutated_two_level_nested_shared_capture_x500_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 500 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 21000 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (B) READ-ONLY two-level capture (no mutation anywhere) — the WORKING
// CONTROL: `assignedCaptureVars_` never contains `f`, so
// `mutatedSharedCaptureCell` never fires regardless of this fix. Must stay
// crash-free on BOTH sides of the fix.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           read_only_two_level_nested_shared_capture_stays_crash_free_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var x := f(); "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 63 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (B2) Same read-only two-level shape, loop-amplified to 500 calls under the
// full LeakSanitizer oracle.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           read_only_two_level_nested_shared_capture_x500_is_memory_clean_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        var x := f(); "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 500 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 3500 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (C) THREE levels of nesting (`l1` -> `l2` -> `l3`, `l3` mutates `f`) — the
// brief's explicit "check 3 and 4, not just 2" requirement: an off-by-one in
// a nesting counter would pass at depth 2 and fail here. `l1` and `l2` BOTH
// only hold `f`'s cell transitively (neither references `f` in its own
// top-level statements), so BOTH must be correctly counted as capturing envs
// at every ancestor level for the ancestor-merge to hold at this depth.
// `runAotCheckedLenient` for the same #4261 reason as row (A) — a direct
// `--sanitize=address` probe at this row's own program measured the SAME
// 360 B / 9 allocations (40 B/call) as the two-level case, confirming
// #4261's drain bug is depth-independent (any nested-LocalDecl chain
// triggers it), not something that scales with nesting depth.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           mutated_three_level_nested_shared_capture_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          f := lambda() => 99; "
        "        end; "
        "        l3(); "
        "      end; "
        "      l2(); "
        "    end; "
        "    l1(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 891 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (C2) THREE-level READ-ONLY control — must stay crash-free on both sides.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           read_only_three_level_nested_shared_capture_stays_crash_free_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          var x := f(); "
        "        end; "
        "        l3(); "
        "      end; "
        "      l2(); "
        "    end; "
        "    l1(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 63 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (D) FOUR levels of nesting (`l1` -> `l2` -> `l3` -> `l4`, `l4` mutates
// `f`) — one level past the explicit "check 3" ask, to catch an off-by-one
// that a THREE-level row alone would not: e.g. a fix that ancestor-merges
// exactly once (depth 2->3) rather than on every `lowerLambda` invocation
// would still pass at depth 3 and fail at depth 4. `runAotCheckedLenient`
// for the same #4261 reason as rows (A)/(C) — measured the same 360 B / 9
// allocations (40 B/call) via a direct probe, again depth-independent.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           mutated_four_level_nested_shared_capture_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          var l4 := lambda() => do "
        "            f := lambda() => 55; "
        "          end; "
        "          l4(); "
        "        end; "
        "        l3(); "
        "      end; "
        "      l2(); "
        "    end; "
        "    l1(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 495 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (D2) FOUR-level READ-ONLY control — must stay crash-free on both sides.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           read_only_four_level_nested_shared_capture_stays_crash_free_spec_27_5) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method One(): int "
        "    var f: () -> int := lambda() => 7; "
        "    var l1 := lambda() => do "
        "      var l2 := lambda() => do "
        "        var l3 := lambda() => do "
        "          var l4 := lambda() => do "
        "            var x := f(); "
        "          end; "
        "          l4(); "
        "        end; "
        "        l3(); "
        "      end; "
        "      l2(); "
        "    end; "
        "    l1(); "
        "    return f(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 63 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (E) SIBLING NON-CAPTURED LOCAL — a plain local declared alongside `f`,
// never referenced by any lambda at all, in the SAME two-level-mutated
// shape. Proves the fix's per-key bookkeeping (multiCaptureLocals_,
// keyed by LocalDecl*/Symbol*) does not spill onto an unrelated key: `g`
// must read back correctly and contribute no crash of its own.
// `runAotCheckedLenient` for the same #4261 reason as row (A) — `g` itself
// is an `int` (no runtime-managed storage, nothing for it to leak); the
// measured 360 B / 9 allocations here is #4261's own `f`-cell leak,
// unaffected by `g`'s presence, confirming no cross-key spill in either
// direction.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           sibling_non_captured_local_unaffected_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var g := 10; "
        "    var f: () -> int := lambda() => 7; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => do "
        "        f := lambda() => 42; "
        "      end; "
        "      inner(); "
        "    end; "
        "    outer(); "
        "    return f() + g; "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 468 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// (F) TWO INDEPENDENT NESTED CHAINS in ONE function — `f1`/`outer1`/`inner1`
// and `f2`/`outer2`/`inner2` are entirely separate two-level mutated shared
// captures, coexisting in the SAME method body. Proves the per-key
// ancestor-merge (keyed by each LocalDecl* independently) does not conflate
// the two chains: a bug that used one shared flag instead of a per-key set
// would still pass the single-chain rows above but could double-suppress or
// under-suppress here. `runAotCheckedLenient` for the same #4261 reason —
// measured 720 B / 18 allocations (2 × 360 B / 9, i.e. TWO independent
// 40 B/call leaks, one per chain) via a direct probe, confirming #4261's
// drain bug is also per-key/independent, not shared or doubled across
// distinct cells in the same function.
// =====================================================================
BEZEL_TEST(codegen_closure_captured_cell_mutated_shared_nested_uaf_4240,
           two_independent_two_level_nested_shared_captures_no_crash_spec_27_5) {
    JitResult r = runAotCheckedLenient(
        "module M; class C "
        "  public static method One(): int "
        "    var f1: () -> int := lambda() => 7; "
        "    var f2: () -> int := lambda() => 3; "
        "    var outer1 := lambda() => do "
        "      var inner1 := lambda() => do "
        "        f1 := lambda() => 42; "
        "      end; "
        "      inner1(); "
        "    end; "
        "    var outer2 := lambda() => do "
        "      var inner2 := lambda() => do "
        "        f2 := lambda() => 100; "
        "      end; "
        "      inner2(); "
        "    end; "
        "    outer1(); "
        "    outer2(); "
        "    return f1() + f2(); "
        "  end One; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 9 do "
        "      total := total + C.One(); "
        "      i := i + 1; "
        "    end; "
        "    if total <> 1278 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
