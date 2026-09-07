// Disposal-correctness — the v0.209 arm-position transfer marker `owned x`
// (issue #2080; spec §28.3 K3.4/K7.4/A7, §20, §19.2 — v0.209).
//
// §28.3 (v0.209): the marked arm is an OWNED producer for the construct's K7
// meet, with CONSUME-IF-SELECTED semantics — "the binding is consumed on the
// path where its arm is selected and joins per K7.4's existing
// maybe-consumed machinery after the construct (per-path scope-end
// disposal)". §28.3's equalization Pattern 2 gains its one-expression form:
// `var x := (if useFresh then new Stream() else owned cached);`.
//
// Each case pins the disposal accounting natively under the checked oracle
// (runAotChecked — ASan/LSan): the WINNER is disposed exactly once by the
// construct's consumer; the LOSER (the not-selected marked binding) is
// disposed exactly once by the K7.4 per-path scope-end disposal (codegen:
// the marker lowering nulls the slot on the selected path; the scope-end
// dispose is null-guarded). A missed loser is a direct leak; a double
// dispose of the winner is a double-free — both flip r.ran under ASan/LSan.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"  // parseAndCheckOwnership / hasErrorMessage — §3591 const-target question below
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

const char* kResClass =
    "module M; "
    "class Res "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Res; ";

} // namespace

// §28.3 Pattern 2 (v0.209): the one-expression coalesce — `(if useFresh then
// new Res(1) else owned cached)`. BOTH paths: the winner disposed once by
// the explicit Dispose; the loser (cached, when the fresh arm is selected)
// disposed once by the per-path scope-end disposal; on the marked path
// cached's slot is nulled so scope end skips it.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_pattern2_coalesce_both_paths) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(useFresh: bool): int "
        "    var cached := new Res(9); "
        "    var x := (if useFresh then new Res(1) else owned cached); "
        "    var v := x.Id; "
        "    x.Dispose(); "
        "    return v; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(true); "
        "    var b := C.Pick(false); "
        "    Console.WriteLine(\"A\" + a.ToString() + \"B\" + b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Pick(true): D1 (explicit) + D9 (per-path scope end). Pick(false): D9
    // (explicit — cached transferred into x). Exactly-once accounting: one
    // D1, two D9 across the program.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9"), 1);
}

// §19.2 (v0.209): expression-`case` arm results — two named owned bindings
// coalesced by arm selection; the winner adopted by a `using var`, the
// loser per-path-disposed at scope end. Both selections exercised.
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_case_arm_results_both_ways) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(n: int): int "
        "    var a := new Res(1); "
        "    var b := new Res(2); "
        "    using var x := (case n of 1 => owned a, _ => owned b); "
        "    return x.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(1); "
        "    var r2 := C.Sel(5); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Each Sel call disposes BOTH Res instances exactly once (winner via the
    // using var, loser via per-path scope end): two D1, two D2 total.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §19.2 (v0.209): `when` arm results — "semantically identical to `case`
// arms for the meet, admitted for parity".
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_when_arm_results) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(c: bool): int "
        "    var a := new Res(1); "
        "    using var x := (when c => owned a, _ => new Res(2)); "
        "    return x.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(true); "
        "    var r2 := C.Sel(false); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Sel(true): D1 via using var (a adopted); no D2 (fresh arm not run).
    // Sel(false): D2 via using var + D1 per-path. Totals: D1 ×2, D2 ×1.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §28.3 K3.4 (v0.209; #2080 review F4): SHADOWING determinism — the marker
// resolves by the SEMA symbol (the same resolution codegen's slot-nulling
// uses), so `owned a` inside a nested block binds the INNER `a`, never the
// same-named outer local an unordered name-scan could hit. Wrong-binding
// resolution manifests as either a spurious discharge error on the inner
// binding or a leak of the outer one — both caught by the exact per-instance
// disposal counts under ASan/LSan on BOTH selections.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_shadowed_binding_resolves_inner) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Shadow(c: bool): int "
        "    var a := new Res(1); "
        "    var acc := 0; "
        "    do "
        "      var a := new Res(2); "
        "      var x := (if c then owned a else new Res(3)); "
        "      acc := acc + x.Id; "
        "      x.Dispose(); "
        "    end; "
        "    return acc; "
        "  end Shadow; "
        "  public static method Main(): int "
        "    var r := C.Shadow(true) + C.Shadow(false); "
        "    Console.WriteLine(\"R\" + r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Shadow(true): inner a(2) transferred+disposed (D2), outer a(1)
    // auto-dropped (D1). Shadow(false): fresh D3, inner a(2) per-path D2,
    // outer D1. Totals: D1 ×2, D2 ×2, D3 ×1; R = 2 + 3.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R5"), 1);
}

// §28.3 K7 (v0.209): the SAME binding marked in both branches — owned ⊓
// owned with one obligation; fully consumed after the construct (MovedFrom),
// disposed exactly once via the adopting consumer on either path.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_same_binding_both_arms) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Both(c: bool): int "
        "    var a := new Res(7); "
        "    using var x := (if c then owned a else owned a); "
        "    return x.Id; "
        "  end Both; "
        "  public static method Main(): int "
        "    var r := C.Both(true) + C.Both(false); "
        "    Console.WriteLine(\"R\" + r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 2);  // one per call
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R14"), 1);
}

// ============================================================================
// fix #4006 (§28.3 K3.4, v0.246 proposal 0090/#3056): the `move x` spelling.
//
// The v0.246 changelog ("the transfer-marker unification") deletes the
// `MarkedTransfer ::= "owned" Identifier` production and unifies the K3.4
// arm-transfer spelling onto plain `move x` — the SAME keyword used
// everywhere else a named owned binding transfers. The current spec (v0.294)
// states K3.4's spelling as `move x` throughout: the consume table's own
// K3.4 row ("the marked arm transfer: `move x` as an if-expression branch or
// an expression-`case` arm result"), the arm-meet row's worked example
// (`var x := (if useFresh then new FileStream(...) else move cached);`),
// and §7.7's canonical two-binding coalescing idiom (`var r := (if a <> null
// then move a else move b);`).
//
// The implementation never migrated ARM POSITION onto this spelling: `owned
// x` above still routes through `processMarkedArmConstruct`'s per-path-safe
// disposal; a bare `move x` in the IDENTICAL position fell through as an
// ordinary (unmarked) borrow-arm meet — semantically wrong but, before fix
// #3989/#3990 gave `recordMovesIn` a `ConditionalExpr`/`CaseExpr` arm,
// ACCIDENTALLY harmless (nothing ever validated or marked the move at all).
// #3989/#3990's fix made `recordMovesIn` reach the pre-existing
// `UnaryExpr(move)` arm for real, genuinely marking `a`/`b` moved-from — and
// the resulting Owned+maybeConsumed join, never having passed through
// `processMarkedArmConstruct`, hit the pre-existing #338/#1626 hard-error
// end-of-scope scan. Measured: `using var r := (if c then move a else move
// b); return r.tag;` compiled and ran to 5 on #3989/#3990's own parent,
// then FAILED TO COMPILE on #3989/#3990 itself — "owned value in `a`/`b`
// would be dropped here without disposal (§28.3)" — breaking
// `checkread_expression_walk_3971`'s own already-passing
// `positive_marked_arm_transfer_still_consumes_exactly_once` in the same
// batch. Filed as issue #4006; fixed here by making `exprContainsMarkedTransfer`
// / `processMarkedArmConstruct` recognize a bare `move <identifier>` arm
// value as the SAME K3.4 marked transfer the `owned x` spelling is, sharing
// the validate+consume core (`consumeMarkedArmTransfer`) rather than a
// second implementation — so both spellings take the identical per-path-safe
// disposal path. `owned x` is NOT removed (a bigger, separate v0.246
// compiler-catch-up question #4006 explicitly declines to resolve here).
//
// These rows mirror the five `owned`-spelled tests above, one per one, with
// `move` substituted — proving the shared core produces IDENTICAL disposal
// accounting under the same ASan/LSan-verified oracle. The
// `spec_28_3_pattern2_*_4016` and `spec_19_2_case_three_arm_*_4016` rows
// below additionally pin fix #4016 (the MIXED move+fresh-producer arm-meet
// gap this #4006 landing's own reviewer found and left unfixed).

// §7.7 / §28.3 (v0.294): the spec's OWN canonical two-binding coalescing
// example, verbatim modulo names: `var r := (if a <> null then move a else
// move b);`. Both selections exercised; each disposes exactly the winner
// (via the `using var`) and the loser (per-path scope end) exactly once.
BEZEL_TEST(marked_arm_transfer_2080, spec_7_7_move_spelled_coalesce_both_paths_4006) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(useA: bool): int "
        "    var a := new Res(1); "
        "    var b := new Res(2); "
        "    using var r := (if useA then move a else move b); "
        "    return r.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(true); "
        "    var r2 := C.Sel(false); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Sel(true): D1 via using var (a adopted), D2 per-path (b never touched
    // but still scope-end disposed once). Sel(false): D2 via using var, D1
    // per-path. Totals: D1 x2, D2 x2.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §28.3 Pattern 2 (v0.209/v0.246), `move` spelling — `(if useFresh then new
// Res(1) else move cached)`, a `move`-marked arm MIXED with an ordinary
// fresh-producer sibling arm. fix #4016: root cause was NOT in the ownership
// pass (which correctly flags `cached`'s LocalDecl `dischargedByArmTransfer`
// for both spellings identically) but in a SECOND, independent copy of the
// "is this arm meet OWNED" classification: `common::armIsOwnedProducer`
// (src/common/OwnershipFacts.cpp), the codegen-and-ownership-shared arm-meet
// authority `unwindOwnedLocalClass` / `usingVarOwnsClassObjectDynamically` /
// `producerState` all consult, recognized `owned x` (MarkedTransferExpr) as
// an owned arm but NOT a bare `move x` — so `x`'s own binding (holding
// WHICHEVER arm ran) was classified a BORROW meet for `move`, an OWNED meet
// for `owned`. An explicit `x.Dispose()` on a "borrow" binding fell to the
// dispatch-only fallback in `explicitDisposeOwnedReceiverSlot`'s caller
// (`emitDynamicDispose`, no reclaim) — the destructor ran (hence
// `cached`'s "D9" appeared in stdout) but the 16-byte block was never
// `free()`d, which is why LSan flagged a leak despite the visible destructor
// print. Fixed by teaching `armIsOwnedProducer` the same bare-`move`-arm
// recognition the ownership pass already had (`common::isBareMoveArmValue`,
// now the ONE shared shape both `MethodOwnership::isBareMoveArmValue` and
// `armIsOwnedProducer` consult). A SECOND, independent bug surfaced by the
// same fix: once `x`'s binding is correctly classified OWNED, the bare
// `move cached` UnaryExpr's own codegen (LlvmBackend_Exprs2.cpp) must null
// `cached`'s slot on the SELECTED path exactly as `owned cached`'s dedicated
// MarkedTransferExpr codegen arm always has — it did not (that unconditional
// null lived ONLY on the MarkedTransferExpr arm, never reached by a bare
// `move`), so selecting the `move` arm left `cached`'s slot non-null and its
// per-path scope-end disposal fired a SECOND time against the
// already-explicitly-disposed object — a heap-use-after-free, measured
// during this fix's own verification. Both are fixed together here.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_pattern2_move_spelled_coalesce_both_paths_4016) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(useFresh: bool): int "
        "    var cached := new Res(9); "
        "    var x := (if useFresh then new Res(1) else move cached); "
        "    var v := x.Id; "
        "    x.Dispose(); "
        "    return v; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(true); "
        "    var b := C.Pick(false); "
        "    Console.WriteLine(\"A\" + a.ToString() + \"B\" + b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Byte-identical accounting to the `owned`-spelled control above
    // (spec_28_3_pattern2_coalesce_both_paths): Pick(true) — D1 (explicit
    // dispose of the fresh winner) + D9 (per-path scope-end dispose of the
    // not-selected `cached`, the #4016 leak this pins). Pick(false) — D9
    // once (explicit dispose of `x`, now holding the moved-in `cached`; the
    // per-path scope-end check sees `cached`'s slot nulled and skips — the
    // #4016 double-free/UAF this also pins).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9"), 1);
}

// #4016, `using var` binding form: the SAME mixed move+fresh construct, but
// the consumer is a `using var` rather than a plain `var` + explicit
// `.Dispose()`. Exercises `usingVarOwnsClassObjectDynamically`'s OWN
// `conditionalCaseArmMeetIsOwned` call site (a different function from
// `unwindOwnedLocalClass`, sharing the same underlying predicate) — the
// #4016 fix must hold for BOTH consumer forms, not just the reported one.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_pattern2_move_spelled_coalesce_using_var_4016) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(useFresh: bool): int "
        "    var cached := new Res(9); "
        "    using var x := (if useFresh then new Res(1) else move cached); "
        "    return x.Id; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(true); "
        "    var b := C.Pick(false); "
        "    Console.WriteLine(\"A\" + a.ToString() + \"B\" + b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Same shape of accounting as the explicit-dispose form above: D1 x1
    // (using-var scope-end dispose of the fresh winner, Pick(true) only),
    // D9 x2 (Pick(true)'s per-path dispose of the not-selected `cached`;
    // Pick(false)'s using-var dispose of the moved-in `cached`).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9"), 1);
}

// #4016, RETURNED form: the mixed move+fresh construct is the method's
// direct `return` value (declared `owned Res`, no intermediate local at
// all) rather than bound to any local in the callee. `cached`'s own
// per-path scope-end disposal (registered against ITS OWN LocalDecl,
// independent of how the construct's result is consumed) must still fire
// correctly with no intervening `explicitDisposeOwnedReceiverSlot` /
// `unwindOwnedLocalClass` call site in the picture at all.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_pattern2_move_spelled_coalesce_returned_owned_4016) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(useFresh: bool): owned Res "
        "    var cached := new Res(9); "
        "    return (if useFresh then new Res(1) else move cached); "
        "  end Pick; "
        "  public static method Main(): int "
        "    using var a := C.Pick(true); "
        "    using var b := C.Pick(false); "
        "    Console.WriteLine(\"A\" + a.Id.ToString() + \"B\" + b.Id.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9"), 1);
}

// #4016, three-arm `case` variant: a `move`-marked arm mixed with TWO
// ordinary fresh-producer sibling arms (not just one) — confirms the fix
// generalizes past the two-arm `if`-expression shape the issue's own repro
// used. Arm 1 and the default arm are fresh producers; arm 2 moves `cached`.
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_case_three_arm_move_mixed_with_fresh_4016) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Pick(n: int): int "
        "    var cached := new Res(9); "
        "    var x := (case n of 1 => new Res(1), 2 => move cached, _ => new Res(3)); "
        "    var v := x.Id; "
        "    x.Dispose(); "
        "    return v; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var a := C.Pick(1); "
        "    var b := C.Pick(2); "
        "    var c := C.Pick(9); "
        "    Console.WriteLine(\"A\" + a.ToString() + \"B\" + b.ToString() + \"C\" + c.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Pick(1): D1 (explicit) + D9 (per-path, cached not selected). Pick(2):
    // D9 once (explicit dispose of x, holding the moved-in cached). Pick(9):
    // D3 (explicit) + D9 (per-path, cached not selected). Totals: D1 x1,
    // D3 x1, D9 x3.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D9"), 3);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "A1B9C3"), 1);
}

// §19.2, `move` spelling: expression-`case` arm results coalescing two named
// owned bindings. Direct analogue of `spec_19_2_case_arm_results_both_ways`.
BEZEL_TEST(marked_arm_transfer_2080, spec_19_2_case_arm_results_move_spelled_both_ways_4006) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Sel(n: int): int "
        "    var a := new Res(1); "
        "    var b := new Res(2); "
        "    using var x := (case n of 1 => move a, _ => move b); "
        "    return x.Id; "
        "  end Sel; "
        "  public static method Main(): int "
        "    var r1 := C.Sel(1); "
        "    var r2 := C.Sel(5); "
        "    Console.WriteLine(\"R\" + r1.ToString() + r2.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 2);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R12"), 1);
}

// §28.3 K7, `move` spelling: the SAME binding moved in both branches — this
// is the coordinator-reported repro's own exact shape
// (`using var best := (if useA then move a else move b);`, generalized to
// `move a`/`move a`). Owned meet owned, one obligation, MovedFrom after the
// construct, disposed exactly once via the adopting consumer on either path.
BEZEL_TEST(marked_arm_transfer_2080, spec_28_3_same_binding_move_spelled_both_arms_4006) {
    auto r = runAotChecked(std::string(kResClass) +
        "class C "
        "  public static method Both(c: bool): int "
        "    var a := new Res(7); "
        "    using var x := (if c then move a else move a); "
        "    return x.Id; "
        "  end Both; "
        "  public static method Main(): int "
        "    var r := C.Both(true) + C.Both(false); "
        "    Console.WriteLine(\"R\" + r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 2);  // one per call
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R14"), 1);
}

// §28.3 K3.4 (v0.209 #2080, extended #4006): a `move` at arm position NESTED
// inside a larger expression (`new Holder(move a)`, not the arm's own bare
// value) is explicitly OUT OF SCOPE for the marked-transfer machinery —
// #3990's own shape, which must keep going through `recordMovesIn`'s
// K7.4-join arm instead. Confirms `isBareMoveArmValue`'s "only the arm's
// own, unwrapped value" boundary: the SAME binding, moved in every arm but
// wrapped in a constructor call each time, still joins to definite
// MovedFrom (no marker machinery involved) with correct disposal counts.
BEZEL_TEST(marked_arm_transfer_2080,
           nested_move_not_bare_arm_value_stays_outside_marked_machinery_4006) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
        "end Res; "
        "class Holder "
        "  public v: int; "
        "  public constructor(owned r: Res) v := r.Id; r.Dispose(); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Both(c: bool): int "
        "    var a := new Res(3); "
        "    using var h := (if c then new Holder(move a) else new Holder(move a)); "
        "    return h.v; "
        "  end Both; "
        "  public static method Main(): int "
        "    var r := C.Both(true) + C.Both(false); "
        "    Console.WriteLine(\"R\" + r.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D3"), 2);  // one per call
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R6"), 1);
}

// #3591 / §21.1 / §28.3 K3.4 (v0.294): the ratchet-review question behind
// this fix's own `isArmTransferDischarge` LOCALDECL-ONLY marker
// (LlvmBackend_Exprs2.cpp) — can a §21.1 `const` local ever legitimately be
// the TARGET of a K3.4 marked arm transfer (`move cached` / `owned cached`)?
// MEASURED: no, on TWO independent, layered grounds, so `consumeMarkedArm
// Transfer`'s LOCALDECL-ONLY restriction to `ast::LocalDecl` never silently
// skips a `const`. (1) §21.1's own Ownership paragraph rejects a `const`
// initializer that is a fresh owned production (`new Res(9)` here) OUTRIGHT,
// at the `const` DECLARATION itself — a `const` local can never even come to
// HOLD an owned class value in the first place. (2) Independently,
// `consumeMarkedArmTransfer`'s own `st->state != OwnershipState::Owned`
// early-return (Ownership_Checks.cpp) would ALSO reject the marker, since a
// `const`-backed symbol's state can never be Owned (§21.1 again) — the SAME
// defensive reasoning already pinned for the `owned` spelling by #3591's own
// `spec_28_3_same_binding_both_arms`-adjacent guard. Confirmed identical for
// all three shapes this row's sibling coalesce/case tests exercise for a
// `var` target: a bare `move` if-arm (pinned below), the `owned` spelling of
// the same, and a `move` `case`-arm — all three produce the SAME two-error
// shape (the §21.1 declaration-site rejection plus the K3.4 "not an owned
// binding at this point" rejection) and zero programs compile, so no
// leak/UAF is reachable through this binding form. Pinning the `move`
// if-arm shape below as the representative row.
BEZEL_TEST(marked_arm_transfer_2080,
           const_target_of_move_arm_transfer_rejected_21_1_28_3_3591) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Pick(useFresh: bool): int "
        "    const cached := new Res(9); "
        "    var x := (if useFresh then new Res(1) else move cached); "
        "    var v := x.Id; "
        "    x.Dispose(); "
        "    return v; "
        "  end Pick; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // (1) the §21.1 declaration-site rejection — a `const` may never take an
    // owned production.
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "must not take an owned production"));
    // (2) the independent K3.4 defensive rejection — even were (1) somehow
    // bypassed, the marker's own operand-state check still refuses.
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "requires a named owned binding"));
}
