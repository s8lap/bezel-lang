// Codegen — issue #4523 / spec §27.5 + §28.9 + §17.13 + §17.15 (v0.306):
// #4509's frame release must be registered into the DECLARING body's dispose
// stack, and a capture-cell holder in any OTHER body must join the refcount
// protocol rather than be declined out of it.
//
// THE DEFECT (#4523). #4509 refcounts a capture cell: `bz_cell_retain` at each
// env's capture-store, `bz_cell_release` in that env's envdtor, and one
// `Disposal::cellRelease` at the declaring scope's end for the frame's own
// implicit reference. Its arming site registered that last entry with
//
//     disposeStack_[declScopeDepth_[decl] - 1].push_back(cellRelease)
//
// where `declScopeDepth_` is MODULE-WIDE and never cleared (`grep -rn
// 'declScopeDepth_' src/ | grep -E '\.clear\(\)|\.erase\('` finds nothing)
// while `disposeStack_` is cleared PER BODY — `LlvmBackend_Exprs2.cpp` for a
// lambda, and again in every async-resume / iterator-moveNext / thunk prolog.
// For a lambda NESTED inside another lambda the capture loop therefore looked
// up the enclosing METHOD's recorded depth and indexed the nested lambda's
// freshly-cleared stack: an extra release into the wrong body's scope. The
// only thing between that and a wild index was the numeric bound
// `<= disposeStack_.size()` — an arithmetic accident, not a proof.
//
// It is NOT a double free. #4509's exclusivity across the six reclaim routes
// holds. It is an OVER-RELEASE: the extra drop takes the count to zero while
// two envs still hold the cell, `bz_cell_release` frees it, and the next read
// through it is a use-after-free — `heap-use-after-free ... in main` under a
// generated-code ASan build, a deterministic SIGSEGV on the plain build, and
// legal spec-conformant code the whole suite was green over.
//
// #4509's guard against this was `!nestedWidenedCellKeys_.count(decl)`, on the
// stated ground that a nested capture site "runs with `currentFn_` set to the
// inner function, where this classification is short-circuited". That ground
// was FALSE — `LlvmBackend_Exprs2.cpp` sets `cellFreeEligibleFn_` to the
// lambda's own function before its body is lowered, so
// `currentFn_ == cellFreeEligibleFn_` HOLDS there and the classification runs.
// What actually declined a nested capture was membership of that set, filled
// by a hand-maintained AST walk in `LlvmBackend_Setup.cpp` that is not
// exhaustive: `ast::BindExpr` (§17.15) and `ast::DiscardStmt` (§17.13) are
// missing from it, and `BindExpr` IS handled by two sibling walkers in the same
// file — the same question answered in more than one place, and answered
// differently.
//
// THE FIX (#4523). Split #4509's single test into the two obligations it
// conflated:
//
//   (1) every env that stores the cell pointer announces itself
//       (retain + envdtor release). That pair is self-balancing, so it is
//       correct in ANY body, nested or not;
//   (2) the declaring frame's implicit reference is dropped once, by a
//       `Disposal::cellRelease` in the declaring scope — and that registration
//       INDEXES `disposeStack_`, so it is gated on `declScopeDepthFn_[decl] ==
//       currentFn_`. Cross-body mis-indexing becomes impossible by
//       construction.
//
// `nestedWidenedCellKeys_` is GONE: with (1) applying everywhere, a nested
// holder is counted rather than declined, which is what that set was reaching
// for and could not express. That deletion is not cosmetic — row 5 measures a
// 1592 B / 199-allocation leak the set was hiding, the same signature #4509
// reports for its own repro.
//
// MEASURED (this worktree, `build/bezelc` rebuilt at each end, 5 runs each):
//   §17.15 binding-atom repro  pre-fix exit 139 (SIGSEGV) 5/5 -> post-fix 2 5/5
//   §17.13 discard repro ..... pre-fix exit 139 (SIGSEGV) 5/5 -> post-fix 7 5/5
//   §27.5 nested-widened loop  pre-fix 1592 B / 199 allocs -> post-fix clean
// The two crash rows emit TWO `bz_cell_retain` sites on BOTH compilers
// (`objdump -d -r`), so the defect there is not a missing retain but a surplus
// release: pre-fix those programs report `heap-use-after-free` under
// `--sanitize=address`, post-fix they are clean under
// `detect_leaks=1:use_stacks=0`.
//
// FAIL-WITHOUT is mechanical: with `src/codegen/` restored to 2151b15a and
// everything else unchanged, the first four rows ran `2 passed, 2 failed` —
// the two crash rows failing on both their assertions — and row 5 was measured
// separately as the leak above. Rows 3 and 4 pass on both compilers by design;
// each says so at its own comment, so nothing here is claimed to pin more than
// it does.
//
// The crash rows run on the PLAIN oracle (`runAot`) deliberately: the fault is
// observable there without instrumentation, and per #2633 an ASan build can
// land a wild access in mapped memory and pass, so the plain build is the
// oracle that reproduces what a user sees.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §24 event-subscription shape #4509 arms on, with a NESTED lambda added
// inside the handler body. `nestedBody` is spliced in ahead of the handler's
// own `box.Add(...)`, so every row here captures `box` from BOTH the handler's
// env (declaring frame = `Main`) and the nested lambda's env (a different
// body) — which is the two-holder configuration #4523 is about.
std::string nestedHandler(const std::string& nestedBody, int fire,
                          int iterations) {
    const std::string open =
        "module M; "
        "record Ev(N: int); "
        "class Box "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box; "
        "class Button "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Button; "
        "class C public static method Main(): int ";
    const std::string body =
        "  using var box := new Box(); "
        "  using var b := new Button(); "
        "  using var sub := b.Clicked.Subscribe(lambda(e) => do " +
        nestedBody +
        "    end); "
        "  b.Fire(" + std::to_string(fire) + "); ";
    if (iterations == 0)
        return open + body + "  return box.V; end Main; end C;";
    // Loop-amplified: one fresh cell per iteration, so a surviving 8 B/capture
    // leak is visible to LSan without relying on its conservative stack scan
    // (the vacuous one-shot-leak oracle CLAUDE.md catalogues).
    return open +
           "  var total := 0; "
           "  for var i in 0..<" + std::to_string(iterations) + " do " +
           body +
           "    total := total + box.V; "
           "  end; "
           "  if total = " + std::to_string(fire * iterations) +
           " then return 7; end; "
           "  return 1; "
           "end Main; end C;";
}

}  // namespace

// §17.15 / §27.5 / §28.9 / #4523 — THE REPRO. The nested lambda sits inside a
// BINDING ATOM (`(var q := …; q)`), one of the two AST shapes #4509's
// nested-widening walk does not visit. Pre-fix the handler's capture loop
// registered the frame's `cellRelease` into the HANDLER's dispose stack using
// `Main`'s recorded depth; that release nulled the cell holder and the next
// read through it segfaulted. `exitCode == 2` is the load-bearing oracle —
// `box.V` after one `Fire(2)` — and `ran` is false pre-fix because the program
// dies on SIGSEGV.
BEZEL_TEST(capture_cell_refcount_wrong_frame_4523,
           nested_lambda_in_binding_atom_does_not_segv_spec_17_15) {
    auto r = runAot(nestedHandler(
        "      var z := (var q := (lambda() => box.V)(); q); "
        "      box.Add(e.N + z - z); ",
        2, 0));
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: terminated by signal 11
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // one Fire(2) reached Box.Add
}

// §17.13 / §27.5 / §28.9 / #4523 — the same defect through the OTHER missing
// AST arm: a nested lambda evaluated by a DISCARD statement (`_ := e;`). This
// row exists because patching the walk arm by arm is whack-a-mole — the fix is
// structural, so it must be pinned on more than the one shape that found it.
BEZEL_TEST(capture_cell_refcount_wrong_frame_4523,
           nested_lambda_in_discard_statement_does_not_segv_spec_17_13) {
    auto r = runAot(nestedHandler(
        "      _ := (lambda() => box.V)(); "
        "      box.Add(e.N); ",
        7, 0));
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: terminated by signal 11
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §27.5 / §28.9 / #4523 — a SAFETY CONTROL, not a bug-pin: it passes on both
// compilers, and it is here because this exact program is the use-after-free
// #4509's commit message cites as its reason for adding
// `nestedWidenedCellKeys_`. #4523 deletes that set, so the row's job is to say
// the UAF has not come back by another door.
//
// `inner` captures `x` through `outer`'s env and ESCAPES both frames (`outer`
// is called twice and one result is returned out of `Run`), so `inner`'s env
// outlives every frame that could release the cell statically — the shape
// where counting one holder and not another is fatal. `runAotChecked`
// instruments the generated code, so a freed cell faults AT the read through
// the escaped closure rather than returning a plausible number, and leak
// detection is on, so a lost cell fails too.
//
// MEASURED on both compilers: `objdump -d -r` finds ZERO `bz_cell_retain`
// sites for this program either way — an earlier static route claims the cell
// here, so #4509's refcount never armed on it and the deleted set was not what
// was holding it back. Row 5 is the shape where the deletion actually changes
// behaviour; this one is the shape where it must not.
//
// It is `codegen_deeply_nested_escaping_lambda_3804`'s
// `outer_called_twice_one_escapes_no_uaf` program raised from
// `runAotCheckedLenient` to the STRICT oracle, measured clean under
// `detect_leaks=1:use_stacks=0` here.
BEZEL_TEST(capture_cell_refcount_wrong_frame_4523,
           nested_escaping_holder_keeps_the_cell_alive_spec_27_5) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Run(): () -> int "
        "    var x := 11; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    var inner1 := outer(); "
        "    var inner2 := outer(); "
        "    var r: int := inner1(); "
        "    return inner2; "
        "  end Run; "
        "  public static method Main(): int "
        "    var escaped := C.Run(); "
        "    var n: int := escaped(); "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);         // a UAF or a leak would fail here
    BEZEL_EXPECT_EQ(r.exitCode, 11);  // both closures still read x == 11
}

// §27.5 / §28.9 / #4523 — THE CONTROL THAT MUST KEEP ARMING. #4509's whole
// value is reclaiming this cell; a "fix" that closed the crash by declining
// everything would still pass the two crash rows above. Loop-amplified to 200
// iterations so the 8 B-per-capture residue #4509 measures is visible to LSan
// on its own rather than through the conservative stack scan, and run on the
// checked oracle so the row fails on a leak, a double free and a
// use-after-free alike — with a nested holder present throughout.
//
// It also passes pre-fix, and deliberately so: declaring `box` INSIDE the loop
// body puts its recorded depth beyond the nested lambda's freshly-cleared
// dispose stack, so #4509's numeric bound `<= disposeStack_.size()` declined
// the mis-registration here by arithmetic accident — the same accident that
// made the defect look shape-specific. Its job is the other direction: that
// the arming survives.
BEZEL_TEST(capture_cell_refcount_wrong_frame_4523,
           nested_holder_present_cell_still_reclaimed_spec_28_9) {
    auto r = runAotChecked(nestedHandler(
        "      _ := (lambda() => box.V)(); "
        "      box.Add(e.N); ",
        2, 200));
    BEZEL_EXPECT_TRUE(r.ran);        // false on a surviving per-iteration leak
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // every iteration's handler ran
}

// §27.5 / §28.9 / #4523 — the SECOND bug this fix closes, and the row
// that pins the deletion of `nestedWidenedCellKeys_`. Here the nested lambda is
// bound to a plain local (`var g := lambda() => box.V;`), a shape the
// §27.5/#1135 nested-widening walk DOES visit — so pre-fix the key landed in
// that set and #4509 declined the cell outright, leaving exactly the 8 B per
// capture it was written to reclaim.
//
// MEASURED, 200 iterations, `--sanitize=address` with
// `ASAN_OPTIONS=detect_leaks=1:use_stacks=0`:
//   pre-fix  `bz_cell_retain` sites: 0 -> 1592 byte(s) leaked in 199 allocation(s)
//   post-fix `bz_cell_retain` sites: 2 -> clean, exit 7
// That is the same 1592 B / 199 signature #4509 measures for its own repro:
// the leak never went away for this shape, it was declined into invisibility.
// With the frame's release gated on `declScopeDepthFn_`, the nested holder can
// be COUNTED instead of declined, and the set has nothing left to do.
//
// Loop-amplified for the reason CLAUDE.md gives: one-shot this program is
// clean on BOTH compilers (LSan's conservative stack scan still reaches the
// last cell), so a single iteration would pass vacuously on either side.
BEZEL_TEST(capture_cell_refcount_wrong_frame_4523,
           nested_widened_cell_is_counted_not_declined_spec_27_5) {
    auto r = runAotChecked(nestedHandler(
        "      var g := lambda() => box.V; "
        "      var z := g(); "
        "      box.Add(e.N + z - z); ",
        2, 200));
    BEZEL_EXPECT_TRUE(r.ran);        // pre-fix: 1592 B / 199 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);  // every iteration's handler ran
}
