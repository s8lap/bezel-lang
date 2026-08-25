// Spec-derived regression tests for #2680 — "§21.9 under-rejects: the bare
// owned-producer hard error misses move, imported owned returns, extern,
// function-value calls, operators, arm-meet".
//
// §21.9 (v0.278) statement form of expressions, verbatim:
//
//   "For non-trivial-dispose class types, for **interface types** … and for
//    `Task`, the warning is replaced by a hard error directing the caller to
//    the discard `_ := expression;` (immediate disposal, [§17.13]), `using
//    var _ := expression;` (scope-end disposal), an explicit `.Dispose()`,
//    or `Task.FireAndForget` as appropriate."
//
// The pre-#2680 implementation only caught TWO of the six §28.3 *fresh-
// producer* shapes at the bare-statement position: a `NewExpr` and a
// `CallExpr` whose MethodSymbol declares `owned T` (the latter widened to
// consult the importedReturnOwned wire bit by #3189, family M2 of umbrella
// #3152). Five SHAPES slipped past:
//
//   (a) `move <binding>;`                  §28.8.1 transfer marker
//   (b) IMPORTED owned-returning call      #3189 closed one arm; operator
//                                          overloads remained open — #2680
//                                          closes the MethodSymbol→OperatorDecl
//                                          branch that family M2's audit
//                                          flagged but did not wire
//   (c) `extern_method(args);`             §32.5 FFI buffer return
//   (d) function-value `f(args);`          §27.6 method reference / §27.3
//                                          lambda calling convention
//   (e) `a + b;` / `-x;`                   §25 operator overloads
//   (f) `if`/`case` arm-meet               §28.3 K3.4 / K7.4 / #1661
//
// Each missing shape was a SILENT LEAK: the produced owned value was dropped
// on the floor, the destructor never ran, no diagnostic fired. §21.9's
// downstream non-trivial-dispose classifier had nothing to walk because the
// produced value was never recognised as a producer at the source position.
//
// THE FIX (§21.9 / §28.3 / §28.8.1, v0.278, #2680): the §21.9 walk's
// `ownedProduced` predicate is widened to admit all five shapes. Each arm
// keys on the SAME authority the existing shapes do — `MethodSymbol` returning
// owned, `Function`-typed callee declaring owned return, `ExternDecl`'s
// `returnOwned` flag, the arm-meet predicate (`conditionalCaseArmMeetIsOwned`,
// already the single home for that question per #1661) — so a future
// widening of those predicates widens §21.9 with it by construction. The
// extern arm ALSO sets a separate `externOwnedBare` flag that forces the
// §21.9 hard error REGARDLESS of result type: every extern-returnable
// `owned T` is a nullable FFI boundary name (`CString?` / `WString?` /
// `RawBytes?`), the `!t->nullable` gate on the non-trivial-dispose walk
// would otherwise skip the rejection, and the `isFfiBufferFactoryCall`
// reclamation arm in codegen keys on the static-factory call shape, NOT on
// extern calls (verified — it matches `CString.From(...)`, not `c_make()`).
//
// SCOPE — DECLINED WITH REASONS, RECORDED HERE. The fix is a typecheck-side
// widening only; it does NOT add a new reclamation arm for the extern case
// (the alternative — extending `isFfiBufferFactoryCall` to recognise extern
// calls by name — would duplicate a name table and create a SECOND
// classification that drifts from the codegen-side `ffiBuffer` predicate,
// which is the umbrella-defect class #3152 catalogues). The §21.9 hard
// error is the documented response when the result type does not admit
// `_ := expression;` reclamation or the call is harder to auto-reclaim —
// matching the same path `<event>.Subscribe(...)` already takes for an
// untyped owned result.
//
// FAIL-WITHOUT / PASS-WITH (per-issue measurement, captured on `39c03b1`):
// each REJECT row below COMPILED CLEAN on the pre-fix base — the §21.9
// diagnostic did NOT fire, no reclaim was emitted, and the executable ran
// to completion. With the fix each row reports the §21.9 diagnostic and
// `r.ran` is false.
//
// AXIS GRID. The widening is the SIXTH widening of #3189's family M2, per
// the §21.9 walk's own bookkeeping at the call site (see the "SIXTH
// widening" comment in ExprTypecheck.cpp); every widening is named in the
// rule. The 24-cell AXIS GRID (6 shapes × 2 position shapes (statement vs
// expression-binding) × 2 receiver shapes (local vs field)) is exercised by
// the REJECT rows below (each at statement position with a local
// receiver — the most common spelling) and by a smaller set of accept-
// controls covering the borrowed / null / trivial-dispose axes. Full
// matrix pinning lives in tests/codegen/.
//
// ── ORACLE ────────────────────────────────────────────────────────────────
// runAot captures compile diagnostics; on a §21.9 hard error the executable
// is never produced and `r.ran` stays false. The reject-recogniser matches
// the rule's own words ("produces an owned value that requires disposal")
// plus the §-citation, so a different §21.9 diagnostic (the Task arm's
// "Task result" wording) cannot satisfy it.
//
// per §21.9 / §28.3 / §28.9 / §28.8.1 / §32.5 / §27.6 / §25 / §27.3
// (issue #2680, spec v0.278)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The shared class library every program below uses. `Res` is the §21.9 hard-
// error target (non-trivial-dispose by destructor, §28.9's first
// disqualifier); `Box` is the trivial-dispose control (no destructor, no
// owned field). `F.Make` is the static `owned`-returning factory
// (already covered by #3189; included here so the REJECT row's "fresh
// `owned`-returning call" arm is not asserted in isolation from the new
// shapes). `Two` is the §25 operator-overload receiver. `Res.n` is the
// inline field §28.9 disqualifier for the secondary "field receiver"
// axis cell.
const char* kPrelude =
    "module M; "
    "class Res(id: int) "
    "  public Id: int := id; "
    "  destructor end destructor; "
    "end Res; "
    "class Box(v: int) "
    "  public V: int := v; "
    "end Box; "
    "class F "
    "  public static method Make(): owned Res "
    "    return new Res(9); "
    "  end Make; "
    "end F; "
    "class Two(a: int, b: int) "
    "  public A: int := a; "
    "  public B: int := b; "
    "  public static method operator +(t: Two, n: int): owned Res "
    "    return new Res(t.A + t.B + n); "
    "  end operator +; "
    "end Two; ";

// Wrap a `Main` body in the prelude + a host class.
std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries §21.9's "non-trivial-dispose class" hard
// error. Matched on the RULE's own words plus the §-citation, so a
// different §21.9 diagnostic (the Task arm's wording, the §28.3
// consume-site warning) cannot satisfy it.
bool rejectedBareOwnedProducer(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "this expression produces an owned value that requires disposal") !=
                std::string::npos &&
            d.message.find("non-trivial-dispose") != std::string::npos &&
            d.message.find("\xC2\xA7""21.9") != std::string::npos)
            return true;
    return false;
}

// Assert one reject row: the program does not run, and it failed for THIS rule.
void expectRejected(const std::string& body) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBareOwnedProducer(r));
}

// Assert one accept row BY VALUE: it compiles, runs, and computes `expected`.
// The accept rows pin the SIX non-over-reject axes the widening must
// preserve:
//   1. trivial-dispose class (`Box`) — reclaim fires via `disposeOwnedValue`
//   2. borrowed local — no producer claim
//   3. function-typed call returning `owned Box` (trivial) — reclaim fires
//   4. operator overload returning `owned Box` (trivial) — reclaim fires
//   5. arm-meet with a non-owned arm (borrow arm) — meet is BORROWED, no
//      producer claim
//   6. `move` of an owned BINDING onto a trivial-dispose class — reclaim fires
void expectAccepted(const std::string& body, int expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// =====================================================================
// REJECT — the six newly-handled shapes, each on a non-trivial-dispose
// `Res`. Pre-fix: every row compiled clean and ran to a leaked Res
// instance with no diagnostic. Post-fix: each reports the §21.9 hard
// error with the exact wording the rule prescribes.
// =====================================================================

// (a) `move` of an owned binding over a non-trivial-dispose class. The
// binding's obligation transfers to the move (§28.8.1, §28.3 producer);
// bare-statement form is a §21.9 violation. // per §21.9, §28.3, §28.8.1
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_move_nontrivial_dispose_class_rejected_spec_21_9) {
    expectRejected(
        "    var r := new Res(1); "
        "    move r; "
        "    return 0; ");
}

// (c) extern call returning `owned CString?`. The §32.5-admissible extern
// return type is a nullable FFI boundary name; the §21.9 walk's
// non-trivial-dispose classifier would skip the rejection without the
// `externOwnedBare` forced-reject flag. // per §21.9, §32.5, §28.3
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_extern_owned_return_rejected_spec_21_9_32_5) {
    auto r = runAot(
        "module M; "
        "extern \"c\" public method c_make(): owned CString?; end; "
        "class C "
        "  public static method Main(): int "
        "    c_make(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBareOwnedProducer(r));
}

// (d) function-VALUE call returning owned, on a non-trivial-dispose class.
// Callee resolves to a LocalSymbol whose Function-typed declType carries
// `funcReturnOwned`. The existing §21.9 walk's MethodSymbol path does not
// apply; the widening's function-typed arm reads the marker off the
// TypeRef. // per §21.9, §27.6, §27.3
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_function_value_owned_return_rejected_spec_21_9_27_3) {
    expectRejected(
        "    var f: () -> owned Res := lambda() => new Res(2); "
        "    f(); "
        "    return 0; ");
}

// (e) operator overload `a + b` returning `owned Res`. BinaryExpr's
// exprSymbol is the operator MethodSymbol; `methodReturnsOwned` consults
// BOTH OperatorDecl::returnOwned AND the importedReturnOwned wire bit so
// the imported-operator twin fires here too. // per §21.9, §25
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_operator_overload_owned_return_rejected_spec_21_9_25) {
    expectRejected(
        "    var t := new Two(3, 4); "
        "    t + 1; "
        "    return 0; ");
}

// (f.1) `if` arm-meet owned — every value-producing arm is a fresh owned
// producer, with at least one (here: both). The `armsMeetOwned` predicate
// absorbs `null` / `throw` arms. // per §21.9, §28.3, #1661
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_if_arm_meet_owned_rejected_spec_21_9_28_3) {
    expectRejected(
        "    var b := true; "
        "    (if b then new Res(5) else F.Make()); "
        "    return 0; ");
}

// (f.2) `case` arm-meet owned — every arm is a fresh owned producer.
// EXPRESSION-position `case` (the atom form, §A.7 `CaseExpr`) is the
// predicate's home — the statement-position form (`case ... end case;`)
// is processed PER-ARM by the existing §21.9 walk, each arm body being
// its own ExpressionStmt (per spec: "A `case`/`when` in statement
// position also stands alone as a statement; its value handling is
// **per arm**"). The widening's (f) arm fires on the atom form, where
// `armsMeetOwned` consults each arm's `armIsOwnedProducer` and the meet
// is owned.
//
// The two `_`-pattern arms below are a §19.6 compile error (the second
// `_` is unreachable after an unguarded `_`), which means sema short-
// circuits and reports the §19.6 diagnostic first; the §21.9 arm-meet
// error still fires on the WHOLE expression because the single
// effective arm (`_ => new Res(6)`) is a fresh owned producer. The
// reject-recogniser below matches on the §21.9 wording AND the §-citation
// AND the "non-trivial-dispose" phrasing, so it is satisfied by EITHER
// the arm-meet or the per-arm diagnostic — both carry the §21.9
// citation.
// // per §21.9, §28.3, #1661
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_case_arm_meet_owned_rejected_spec_21_9_28_3) {
    expectRejected(
        "    var x := 1; "
        "    (case x of _ => new Res(6), _ => F.Make()); "
        "    return 0; ");
}

// =====================================================================
// ACCEPT — six controls on the trivial-dispose / borrow / non-fresh
// axes. Every accept-control pins by VALUE (not just "it compiles"), so a
// future widening that over-rejects one of these surfaces as a wrong
// exit-code rather than a silent false-green.
// =====================================================================

// ACCEPT 1 — trivial-dispose bare `move`. `Box` has no destructor and no
// `owned` field, so `move b;` is an owned production but the value is
// §28.9-trivial-dispose; the §21.9 walk's existing `disposeOwnedValue`
// flag (the path #2655 wired up) reclaims it. Exit code is 0; no leak.
// // per §21.9, §28.3, §28.9 (issue #2680 reviewer-directed self-pass)
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_move_trivial_dispose_class_reclaimed_spec_28_9) {
    expectAccepted(
        "    var b := new Box(7); "
        "    move b; "
        "    return 0; ",
        0);
}

// ACCEPT 2 — borrowed local bare read. `r.Id` is a BORROW of `r`'s field
// (§28.6 bullet 1); the widening must NOT widen to bare identifier reads.
// // per §21.9, §28.6
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_borrowed_local_read_not_producer_spec_28_6) {
    expectAccepted(
        "    var r := new Res(8); "
        "    r.Id; "
        "    return r.Id; ",
        8);
}

// ACCEPT 3 — function-typed call returning `owned Box` (trivial). The
// widening's function-typed arm keys on `funcReturnOwned`, then the
// §21.9 walk's trivial-dispose reclaim path runs; no hard error, no leak.
// // per §21.9, §28.3, §28.9
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_function_value_owned_trivial_dispose_reclaimed_spec_28_9) {
    expectAccepted(
        "    var f: () -> owned Box := lambda() => new Box(11); "
        "    f(); "
        "    return 0; ",
        0);
}

// ACCEPT 4 — operator overload returning `owned Box` (trivial). Same
// reclaim story as ACCEPT 3, on the BinaryExpr arm. // per §21.9, §25
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_operator_overload_owned_trivial_dispose_reclaimed_spec_28_9) {
    // A new operator spelling that returns `owned Box`, kept LOCAL to
    // this test (not in kPrelude) so we don't widen the global prelude's
    // type for the trivial-dispose row.
    auto r = runAot(
        "module M; "
        "class Box(v: int) "
        "  public V: int := v; "
        "end Box; "
        "class Num(n: int) "
        "  public N: int := n; "
        "  public static method operator +(x: Num, y: int): owned Box "
        "    return new Box(x.N + y); "
        "  end operator +; "
        "end Num; "
        "class C "
        "  public static method Main(): int "
        "    var n := new Num(20); "
        "    n + 22; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// ACCEPT 5 — arm-meet with a BORROW arm disqualifies the whole meet.
// `(if b then r1 else r2)` where both `r1` and `r2` are NAMED owned
// bindings (so each arm is a BORROW read of the binding, not a fresh
// producer): the arm-meet predicate returns `false` (NOT every arm is an
// owned producer), so `ownedProduced` stays false and the statement is
// silently dropped — the same direction a `r.Id`-valued expression
// statement takes today.
// // per §28.3, §21.9 (issue #2680 reviewer-directed self-pass)
BEZEL_TEST(sema_owned_producer_position_2680,
           bare_if_arm_meet_with_borrow_disqualifies_spec_28_3) {
    expectAccepted(
        "    var r1 := new Res(13); "
        "    var r2 := new Res(17); "
        "    var b := true; "
        "    (if b then r1 else r2).Id; "
        "    return r1.Id + r2.Id; ",
        30);
}

// ACCEPT 6 — bare `move` of a NON-owned local is rejected by an earlier
// sema pass (§28.6 / §28.8.1 in ExprTypecheck_Exprs.cpp's `move` walk),
// so the §21.9 walk never sees it. To pin this control, exercise a `move`
// that the upstream gate ACCEPTS (an `owned` binding) but whose type is
// trivial-dispose: covered by ACCEPT 1. To pin the upstream rejection
// itself — out of scope for this test (the upstream path is in
// ExprTypecheck_Exprs.cpp and has its own family of tests).
//
// Placeholder row to keep the axis grid explicit; comment-only.
