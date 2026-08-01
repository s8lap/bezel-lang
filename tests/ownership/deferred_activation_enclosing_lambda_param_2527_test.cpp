// Ownership tests for #2527 (§28.3 "capture-set contribution", §28.14 frame
// residence, v0.196; sibling of #2342 §28.14).
//
// The §28.3 deferredActivationCaptureSources collector's parameter-reduction
// gate (Ownership_Checks.cpp:9177, `deferredActivationCaptureSources`) used
// `methodParamSet_` (seeded in Ownership.cpp:144/:1111 from the enclosing
// METHOD's own formals) to skip "lambda-internal" parameters. That conflated
// two cases — a parameter of the FORWARDED lambda itself (correctly skipped,
// not a capture) and a parameter of an ENCLOSING LAMBDA (wrongly skipped, IS
// a capture). The §28.3 sibling of #2342's §28.14 shape: an outer lambda
// takes an `owned` parameter; an inner closure (passed in a deferred-
// activation frame-residence position) captures that parameter.
//
// IMPORTANT — UNOBSERVABLE THROUGH CURRENT VISITOR:
// `checkDeferredActivationTempSitesExpr` (the §28.3 deferred-activation
// visitor, Ownership_Checks.cpp:9765) explicitly does NOT descend into lambda
// bodies: "A LambdaExpr is its own scope — its calls are checked when its
// body is analysed, not here." So the §28.3 deferred-activation capture check
// is currently unreachable for deferred-activation calls written inside an
// outer lambda's body. The bug shape is therefore dormant — the gate change
// is mechanically identical to #2342's pre-fix state and is load-bearing for
// any future visitor that descends into lambda bodies, but cannot be
// triggered with the current visitor. The tests below cover the structurally-
// equivalent METHOD-level shape (which IS observable) and verify the fix does
// not regress the deferred-activation capture-set check on either axis:
// (a) the FIX keeps the existing rejection of a method-level `owned` param
//     captured by a forwarded lambda (the `owned_param_capture_rejected` row
//     in the sibling 1818 suite), so no over-admit; and
// (b) the FIX keeps every ACCEPT in the sibling 1818 suite green, so no
//     over-reject — particularly the §28.3 ruling that a PLAIN (borrow)
//     parameter forwarded into a deferred-activation frame is admitted
//     (the caller re-establishes the frame's constraint, §28.3) and that a
//     static-method reference contributes nothing (`Program.Helper`).
//
// The §28.3 captured-sibling fix (the gate change itself) is verified
// mechanically: reverting `Ownership_Checks.cpp` (scratch-copy restore — no
// git stash) keeps every cell in this file passing, because the visitor
// never reaches the deferredActivationCaptureSources gate on a lambda-body
// forwarded lambda — the gate change has no observable behavioural effect
// today. That is the dormancy: structurally correct, observationally null.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// True when a capture-set-contribution diagnostic (the #1818 T2 error) is
// present — pins THIS bug rather than any incidental error.
bool sawCaptureSetError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("capture-set contribution") != std::string::npos)
            return true;
    return false;
}

// Shared preamble: Scaler (owned-disposable), Program.Doubled (the §28.3
// deferred-activation iterator factory over a plain `(int) -> int` param,
// §28.14 v0.196), plus Program.Scale / Program.Factor / Program.Helper so
// tests that exercise `this.Scale` / `Program.Factor` reduce cleanly (the
// receiver-mediated `this` and the static ref `Program.Factor` contribute
// nothing — exactly as the §28.3/§28.14 ruling names).
std::string wrap(const std::string& body, const std::string& sig) {
    return std::string(
        "module M; "
        "class Scaler "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public method Apply(x: int): int => x * 2; "
        "  public method Test(x: int): bool => x > 0; "
        "end Scaler; "
        "class Program "
        "  public static Factor: int := 3; "
        "  public method Scale(x: int): int => x + 1; "
        "  public static method Helper(x: int): int => x + 1; "
        "  public static method Doubled(nums: Array<int>, f: (int) -> int): "
        "      IEnumerable<int> "
        "    for var x in nums do yield return f(x); end; "
        "  end Doubled; "
        "  public ") + sig + " " + body + " end M; "
        "end Program;";
}

}  // namespace

// ===========================================================================
// REGRESSION GUARDS — verify the fix does not change the observable
// behaviour of the structurally-equivalent METHOD-level cases. The deferred-
// activation visitor fires for calls at the method body level, so these
// cases are the only ones currently reachable; the test asserts the FIX
// keeps the existing reject (no over-admit) and keeps the existing accepts
// (no over-reject).
// ===========================================================================

// #2527 guard (existing reject preserved): the method-level `owned` param
// captured by a lambda forwarded to a deferred-activation callee. The OLD
// gate correctly skipped `s` only because `s` is in `methodParamSet_` (it's
// `M`'s own param); the NEW gate skips only `forwardedInternalParams` (the
// forwarded lambda's own params, `x`), so `s` is still classified as an
// owned capture and the reject still fires. // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_enclosing_lambda_param_2527,
          method_level_owned_param_capture_still_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, lambda(x) => s.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>, owned s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// #2527 guard (existing accept preserved): the method-level PLAIN param
// (a borrow) captured by a lambda forwarded to a deferred-activation
// callee. The OLD gate skipped `s` as "not in `methodParamSet_`" — wait,
// `s` IS in `methodParamSet_` (it's `M`'s own param), so the OLD gate
// classifies `s` as a real capture with modifier == "" (not owned). The
// NEW gate classifies `s` the same way (not in `forwardedInternalParams`).
// Either way: admitted (the caller re-establishes the frame's constraint
// against its own argument, §28.3). Guards against the fix wrongly
// rejecting the plain-borrow case. // per §28.3, v0.196
BEZEL_TEST(deferred_activation_enclosing_lambda_param_2527,
          method_level_borrow_param_capture_still_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, lambda(x) => s.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>, s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2527 guard (existing accept preserved): a forwarded lambda whose body
// uses only a method-scope source that contributes nothing (`this.Scale` +
// a value-typed local + a static field). The forwarded lambda's own params
// are classified as INTERNAL (skipped) by both gates — but with the NEW
// gate, the only skipped params are exactly the forwarded lambda's own
// params (`x`), so any method-scope captures are correctly classified.
// Guards against the fix wrongly skipping method-scope captures that
// should reach the reduction. // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_enclosing_lambda_param_2527,
          method_level_capture_free_lambda_still_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var bump: int := 5; "
        "var q := Doubled(nums, lambda(x) => this.Scale(x) + bump + Program.Factor); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2527 guard (existing accept preserved): the forwarded lambda's own
// param is correctly INTERNAL — not a capture — under the NEW gate. The
// forwarded lambda `(x: int) => x` has no captures, so the admitted result
// is empty and the frame carries no owned obligation, even though the
// frame is returned. Both gates skip `x` (old: not in `methodParamSet_`;
// new: in `forwardedInternalParams`), so `x` is never pushed to `out`. The
// load-bearing accept control: the NEW gate must continue to treat the
// forwarded lambda's own param as INTERNAL, exactly as the OLD gate did,
// even though the gate's mechanism has changed. // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_enclosing_lambda_param_2527,
          forwarded_lambda_own_param_still_treated_as_internal) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, lambda(x: int) => x); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Dormancy note — the §28.3 deferred-activation visitor
// (`checkDeferredActivationTempSitesExpr`, Ownership_Checks.cpp:9765) does
// NOT descend into lambda bodies. The "outer lambda's owned param captured
// by an inner closure forwarded to a deferred-activation call" shape is
// therefore NOT observable as a reject on the current visitor. The fix
// below would be needed only if the visitor ever descended — see the
// `deferredActivationCaptureSources` comment block at line 9162.
// ===========================================================================

// #2527 dormancy witness: an outer lambda's `owned` param captured by an
// inner closure forwarded to Doubled INSIDE the outer lambda's body. The
// deferred-activation visitor does not descend into the outer lambda's
// `do … end` block, so the inner Doubled call is never visited, so the
// capture-set-contribution reject never fires — the program currently
// compiles clean. This cell is the dormancy witness: it documents the
// observable behaviour (admitted) so a future change that descends into
// lambda bodies can re-evaluate this case and observe a change. The fix
// in `Ownership_Checks.cpp` is structurally correct regardless of this
// dormancy — it just hasn't become load-bearing yet. // per §28.3, §28.14
BEZEL_TEST(deferred_activation_enclosing_lambda_param_2527,
          outer_lambda_owned_param_captured_in_inner_deferred_call_dormant) {
    auto r = parseAndCheckOwnership(wrap(
        "var outer := lambda(owned s: Scaler) => do "
        "  var q := Doubled(nums, lambda(x) => s.Apply(x)); "
        "  return q; "
        "end; "
        "return outer(new Scaler());",
        "method M(nums: Array<int>): IEnumerable<int>"));
    // Dormancy: the visitor does not descend into `outer`'s body, so the
    // capture-set reject is not reached today. The program compiles clean.
    // A future visitor that descends into lambda bodies would observe this
    // shape change with the fix in place.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
