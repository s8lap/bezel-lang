// Ownership tests for issue #3320 — v0.273 §28.14 admission freeze interval
// scoping. The v1 bound kept the freeze for the rest of the enclosing
// method; v0.273 narrows the freeze to L's block, restoring the `do … end`
// lift idiom Guide 1 §10 and stdlib §2.21 teach.
//
// Spec authority: §28.14, "Until `L`'s scope exits, the captured local
// sources are **frozen** … after `L`'s scope exits the sources are
// unfrozen" — v0.273 corrected the v1 bound that recorded the
// implementation's method-wide freeze as normative. The compiler-side
// change: `ScopedFreeze::scopeEndLoc` is the end of L's enclosing block,
// and `checkScopedFreeze` rejects mutation only in the interval
// [admissionLoc.offset, scopeEndLoc.offset). Test 3 (the loop case, bound
// 2(b)) is NOT implemented in this lane — #2814:169 calls capturing a
// moved-from local a pre-existing general capture gap and asserts no
// error, contradicting bound 2(b)'s justification; the loop case is filed
// separately.

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
// `Res.Mutate()` is the post-fix ACCEPT shape (a member call after L's
// scope has exited, when the captured source is no longer frozen); the
// `freeze_*_rejected` tests exercise the in-scope shape whose
// `frozen` diagnostic must still fire. Runner is the standard
// scoped-storage fixture so the freeze map is populated via `Install`.
std::string prog(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  public method Ping() end Ping; "
        "  public method Mutate() end Mutate; "
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
// Test 1 (ACCEPT): §28.14 v0.273 — the bounding local's block closes, then
// the captured source is MOVED (the freeze's reject action). This is the
// shape the `do … end` lift idiom (Guide 1 §10, stdlib §2.21) admits and
// the v1 method-wide freeze wrongly rejected. The body uses a `do … end`
// so L (`run`) is a `using var` declared inside the block — its
// scope-exit offset is the END of the do-block, NOT the method body.
// After the `end;` of the `do`, the captured source `res` is unfrozen;
// `Prog.Sink(owned res)` is accepted. // per §28.14 (v0.273)
//
// PINS THE BUG: `checkScopedFreeze` is only consulted on move / dispose /
// reassign (Ownership.cpp:5529, Ownership_Checks.cpp:5789 / 6450 / 15746).
// A member call like `res.Mutate()` would NOT trigger the freeze
// diagnostic under either reading, so the test must use a real
// freeze-action (move via `owned x` argument) for the rejection to fire
// under v1. This is the only way to surface the over-restrictive
// method-wide bound in a parse-time check.
BEZEL_TEST(scoped_freeze_3320, do_end_lift_unfreezes_after_block_exit_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    do "
        "      using var run := new Runner(lambda() => res.Ping()); "
        "    end; "
        "    Prog.Sink(move res); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Test 2 (REJECT, control): a mutation of the captured source WHILE the
// bounding local's block is still open — rejected with the freeze
// diagnostic. The interval test MUST keep this shape failing; otherwise
// the narrowing has become a hole.
BEZEL_TEST(scoped_freeze_3320, mutation_in_open_block_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    using var run := new Runner(lambda() => res.Ping()); "
        "    Prog.Sink(owned res); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// Test 2b (REJECT, control, in-do-block): the same in-scope mutation, but
// with the bounding local in a `do … end` block. The mutation is BEFORE
// the block closes, so the freeze still applies. Pin: the diagnostic
// names the captured source (`res`) and the freeze's binding (`run`).
BEZEL_TEST(scoped_freeze_3320, mutation_inside_do_block_while_l_open_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    do "
        "      using var run := new Runner(lambda() => res.Ping()); "
        "      Prog.Sink(owned res); "
        "    end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// ===========================================================================
// Tests 3a-3d (ACCEPT, controls): the §28.14 freeze tests'
// shapes (flat bodies where the v1 method-wide reading and the v0.273
// block-scoped reading coincide) — recreated here for posterity. All must
// remain ACCEPT under the fix: the captured source's mutation happens in
// a DIFFERENT lexical scope (no `using var` runner has the source frozen),
// or the freeze shape simply isn't triggered. None should move.
// ===========================================================================

// Test 3a — ctor form + sources declared before the `using var`: the
// Runner is constructed with a captured lambda; the captured source is
// used (Ping) but not mutated after admission. Flat body — v1 and v0.273
// coincide. ACCEPT.
BEZEL_TEST(scoped_freeze_3320, ctor_form_flat_body_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var run := new Runner(lambda() => res.Ping()); "
        "    run.Fire(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Test 3b — receiver form, same flat-body shape. ACCEPT.
BEZEL_TEST(scoped_freeze_3320, receiver_form_flat_body_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var run := new Runner(lambda() => ()); "
        "    run.Install(lambda() => res.Ping()); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Test 3c — capture-free lambda in the scoped parameter position: there
// is no captured source, so the freeze map is empty and no diagnostic
// fires. Flat body. ACCEPT.
BEZEL_TEST(scoped_freeze_3320, capture_free_flat_body_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    var run := new Runner(lambda() => ()); "
        "    run.Install(lambda() => ()); "
        "    run.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Test 3d — lambda-literal local that's never reassigned is verifiable
// (#1420 widening); capture-free lambda is forwarded to a `scoped`
// parameter. Flat body. ACCEPT.
BEZEL_TEST(scoped_freeze_3320, lambda_literal_local_capture_free_flat_accepted) {
    auto r = parseAndCheckOwnership(prog(
        "    var f := lambda() => (); "
        "    using var run := new Runner(lambda() => ()); "
        "    run.Install(f); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
