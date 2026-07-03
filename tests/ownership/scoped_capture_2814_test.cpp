// Ownership tests for §28.14 — scoped callback parameters: storage-bounded
// capture (v0.175, #808).
//
// §28.14's two-sided contract, tested rule by rule:
//
//  CALL-SITE ADMISSION — "A closure argument capturing locals or parameters
//  is admitted iff the bounding object resolves to a `using var`/`await using
//  var` local L (constructor→constructed / owned-return-as-initializer /
//  receiver) and every captured source is declared BEFORE L … Until L's scope
//  exits, the captured sources are FROZEN: they may not be moved from or
//  reassigned … A bounding object that is none of these admits NO capture-
//  bearing arguments (capture-free callbacks are always admitted)."
//
//  CALLEE VERIFICATION — "The callee may store the callback, but only in
//  `scoped` fields of the bounding object itself, or forward it to another
//  `scoped` parameter whose bounding object is the same object; it may invoke
//  it. It may not return it, move it into non-`scoped` storage, or store it
//  in any other object. A `scoped` field … may be invoked, overwritten, or
//  cleared, but never read out as an escaping value."
//
// Fixture: `Res` (a disposable resource — the captured source), `Runner` (a
// user callee with a `scoped` ctor/method parameter stored in its own scoped
// field — the §28.14 storage shape).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Res + Runner fixture; `body` is spliced in as Prog's Main body.
std::string prog(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Res; "
        "class Runner "
        "  private scoped _cb: (() -> ())?; "
        "  public constructor(scoped cb: () -> ()) _cb := cb; end; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Fire() "
        "    if _cb <> null then _cb(); end; "
        "  end Fire; "
        "  destructor end; "
        "end Runner; "
        "class Prog "
        "  public static method Sink(owned x: Res) x.Dispose(); end Sink; "
        "  public static method Main(): int ") + body +
        "  end Main; "
        "end Prog;";
}

}  // namespace

// ===========================================================================
// Call-site admission
// ===========================================================================

// §28.14 form 1 (constructor → constructed object): a capture of a source
// declared BEFORE the bounding `using var` is ADMITTED, and the callee's
// store into its own scoped field verifies — the whole spec shape compiles.
BEZEL_TEST(scoped_capture_2814, ctor_form_sources_before_scope_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var run := new Runner(() => res.Ping()); "
        "    run.Fire(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 form 3 (receiver): a `using var` receiver bounds the capture; the
// source declared before it is admitted.
BEZEL_TEST(scoped_capture_2814, receiver_form_sources_before_scope_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var run := new Runner(() => ()); "
        "    run.Install(() => res.Ping()); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: DECLARATION-ORDER violation — the source is declared AFTER the
// bounding local, so it dies before the drain; rejected NAMING the capture.
BEZEL_TEST(scoped_capture_2814, declaration_order_violation_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var run := new Runner(() => ()); "
        "    using var res := new Res(); "
        "    run.Install(() => res.Ping()); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "`res`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "declared before"));
}

// §28.14: a bounding object that is NOT a using-var local (a plain owned
// local receiver) admits NO capture-bearing arguments.
BEZEL_TEST(scoped_capture_2814, non_using_var_bounding_rejects_captures) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var run := new Runner(() => ()); "
        "    run.Install(() => res.Ping()); "
        "    run.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// §28.14: "capture-free callbacks are always admitted" — the same non-using-
// var bounding shape with a capture-free callback is clean.
BEZEL_TEST(scoped_capture_2814, capture_free_admitted_without_bounding) {
    auto r = parseAndCheckOwnership(prog(
        "    var run := new Runner(() => ()); "
        "    run.Install(() => ()); "
        "    run.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 FREEZE: a captured source may not be MOVED FROM after the admission
// (it must outlive the bounding scope's drain).
BEZEL_TEST(scoped_capture_2814, freeze_move_of_captured_source_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    using var run := new Runner(() => res.Ping()); "
        "    Prog.Sink(owned res); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// §28.14 FREEZE: a captured source may not be DISPOSED before the scope exits
// (disposal is a discharge — "moved from" in the freeze's sense).
BEZEL_TEST(scoped_capture_2814, freeze_dispose_of_captured_source_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    using var run := new Runner(() => res.Ping()); "
        "    res.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// §28.14 FREEZE: a captured source may not be REASSIGNED after the admission.
BEZEL_TEST(scoped_capture_2814, freeze_reassign_of_captured_source_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    using var run := new Runner(() => res.Ping()); "
        "    res := new Res(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// FREEZE control: a move BEFORE the admission site is the ordinary §28.3
// regime, not a freeze violation (the freeze starts at the admission).
// The capture then names a moved-from local — a pre-existing general capture
// gap outside §28.14's admission rule — so no §28.14 freeze diagnostic fires.
BEZEL_TEST(scoped_capture_2814, move_before_admission_not_frozen) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    Prog.Sink(owned res); "
        "    var res2 := new Res(); "
        "    using var run := new Runner(() => res2.Ping()); "
        "    run.Fire(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(!diagContains(r, "frozen"));
}

// §28.14: a function VALUE whose captures cannot be enumerated at the call
// site is rejected (pass the lambda literal directly).
BEZEL_TEST(scoped_capture_2814, function_value_arg_rejected_unverifiable) {
    auto r = parseAndCheckOwnership(prog(
        "    var f := () => (); "
        "    using var run := new Runner(() => ()); "
        "    run.Install(f); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot be verified"));
}

// §28.14 / §31.1: a catch-bound exception variable may not be captured into a
// stored callback (the runtime disposes it when the arm completes).
BEZEL_TEST(scoped_capture_2814, catch_borrow_capture_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var run := new Runner(() => ()); "
        "    try "
        "      Prog.Sink(new Res()); "
        "    catch "
        "      var ex: Exception => run.Install(() => ex.Message.Length); "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Callee verification
// ===========================================================================

// §28.14: the scoped param may not be RETURNED.
BEZEL_TEST(scoped_capture_2814, callee_return_of_scoped_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method A(scoped cb: () -> ()): () -> () "
        "    return cb; "
        "  end A; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "returned"));
}

// §28.14: the scoped param may not be moved into NON-scoped storage (a local).
BEZEL_TEST(scoped_capture_2814, callee_bind_scoped_param_to_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method B(scoped cb: () -> ()) "
        "    var g := cb; "
        "    g(); "
        "  end B; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.14 v1 tightening: storage only into a scoped field of the bounding
// object ITSELF — another object's scoped field is rejected.
BEZEL_TEST(scoped_capture_2814, callee_store_into_other_object_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Other "
        "  public scoped _slot: (() -> ())?; "
        "  destructor end; "
        "end Other; "
        "class C "
        "  public method S(scoped cb: () -> (), o: Other) "
        "    o._slot := cb; "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "ITSELF"));
}

// §28.14: forwarding to a scoped parameter of a call on `this` (same bounding
// object) is admitted — both the explicit `this.` and the bare-name form.
BEZEL_TEST(scoped_capture_2814, callee_forward_on_this_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Inner(scoped cb: () -> ()) _cb := cb; end Inner; "
        "  public method OuterA(scoped cb: () -> ()) this.Inner(cb); end OuterA; "
        "  public method OuterB(scoped cb: () -> ()) Inner(cb); end OuterB; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: forwarding to ANOTHER object's scoped parameter is rejected — the
// bounding object differs.
BEZEL_TEST(scoped_capture_2814, callee_forward_to_other_object_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Inner(scoped cb: () -> ()) _cb := cb; end Inner; "
        "  public method Bad(scoped cb: () -> (), other: C) "
        "    other.Inner(cb); "
        "  end Bad; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "same bounding object"));
}

// §28.14: a scoped FIELD is never readable-out as an escaping value.
BEZEL_TEST(scoped_capture_2814, scoped_field_read_out_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method R(): (() -> ())? "
        "    return _cb; "
        "  end R; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "never read out"));
}

// §28.14 controls: a scoped field may be INVOKED (behind a null test),
// OVERWRITTEN from a scoped parameter, and CLEARED to null.
BEZEL_TEST(scoped_capture_2814, scoped_field_invoke_overwrite_clear_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Set(scoped cb: () -> ()) _cb := cb; end Set; "
        "  public method Clear() _cb := null; end Clear; "
        "  public method Fire() "
        "    if _cb <> null then _cb(); end; "
        "  end Fire; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a scoped field may only be ASSIGNED a scoped parameter or null —
// an arbitrary lambda (whose captures were never admitted against this
// object's bound) is rejected.
BEZEL_TEST(scoped_capture_2814, scoped_field_assign_arbitrary_lambda_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  private _n: int; "
        "  public method Bad() "
        "    _cb := () => _n; "
        "  end Bad; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
