// Spec-derived tests for issue #4709 — rule-corpus s27_5_0006 / s27_5_0007.
//
// Spec basis: §27.5 "Closures" cites §28.3's closure-source-set rule (v0.298,
// ruled under #4154's closing comment, commit e69757bb; restated v0.307):
//
//   "A closure is a borrow-carrying value ... a lambda literal ... contributes
//   the sources of the borrows it captures — a captured borrowed local or
//   parameter [contributes] that binding's source ... [a function value] may
//   not be stored in a field, in any collection, or through an out/ref
//   parameter, and may not be returned from the enclosing method, while its
//   source set is non-empty ... a function type announces no source set
//   (K2.4), so no holder outside this body can re-establish it and no caller
//   could bound it."
//
//   A PLAIN (non-`owned`/`ref`/`out`/`scoped`) class/interface-typed
//   parameter's own source is "this call": the parameter is a borrow the
//   caller extends only for the duration of the call (the same call-bound
//   premise §22.3 states for a plain function-typed parameter, applied here
//   to a class/interface-typed one). A closure that captures one and escapes
//   via RETURN or FIELD-STORE necessarily outlives that source.
//
// #4154's closing comment ruled this sentence into the spec with "For the
// compiler: nothing new is owed" — but that claim is refuted by direct
// measurement: on r19-c23 @ 9a3c99ed, BOTH rule-corpus repros below compiled
// with rc=0 and zero diagnostics. The existing machinery
// (`isRefOutParam`/`checkRefOutLambdaCaptureOnStore`'s and
// `checkClosureEscapesOnReturn`'s ref/out legs) covers only `ref`/`out`
// PARAMETER captures — a dangling-STACK-POINTER concern (§27.5/§28.6/#1217)
// — and the §9.7 / §28.3-owned-capture legs cover only borrow-bearing
// records/spans/choices and owned locals respectively. A captured PLAIN
// class/interface-typed borrow parameter — a dangling-HEAP-REFERENCE concern
// once the caller's own borrow ends, not a stack pointer — reached none of
// them.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A Widget-shaped receiver with a borrow-only method, matching the rule-
// corpus repro's shape exactly (s27_5_0006 / s27_5_0007).
std::string wrapMethod(const std::string& method) {
    return "module M; "
           "class Widget "
           "  public constructor() end constructor; "
           "  public method Greet() Console.WriteLine(\"hi\"); end Greet; "
           "end Widget; "
           "class C " +
           method + " end C;";
}

// Same, plus a non-capturing function-typed field for the field-store tests.
std::string wrapWithField(const std::string& method) {
    return "module M; "
           "class Widget "
           "  public constructor() end constructor; "
           "  public method Greet() Console.WriteLine(\"hi\"); end Greet; "
           "end Widget; "
           "class C "
           "  private _cb: () -> () := lambda() => Console.WriteLine(\"idle\"); "
           + method + " end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE — a closure capturing a plain borrow parameter must be rejected
// when it escapes via return or field-store (§27.5 / §28.3, v0.298/v0.307).
// =============================================================================

// §27.5 / §28.3 (v0.298/v0.307, #4709): rule-corpus s27_5_0007, the RETURN
// leg. `w: Widget` is a plain borrow parameter; the returned closure's
// source set is `{w}`, non-empty, and a function type announces no source
// set — no caller could bound the escaping borrow.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           return_lambda_capturing_plain_param_rejected_s27_5_0007) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method Make(w: Widget): () -> () "
        "  return lambda() => w.Greet(); "
        "end Make;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "returned closure captures `w`"));
}

// §27.5 / §28.3 (v0.298/v0.307, #4709): rule-corpus s27_5_0006, the
// FIELD-STORE leg.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           store_lambda_in_field_capturing_plain_param_rejected_s27_5_0006) {
    auto r = parseAndCheckOwnership(wrapWithField(
        "public method Capture(w: Widget) "
        "  _cb := lambda() => w.Greet(); "
        "end Capture;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "stored in a field"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captures `w`"));
}

// §27.5 / §28.3: the write-through-out-param leg, the third escape route
// §28.3's rule names alongside field-store and return.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           write_lambda_through_out_param_capturing_plain_param_rejected) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method Give(w: Widget, out cb: () -> ()) "
        "  cb := lambda() => w.Greet(); "
        "end Give;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captures `w`"));
}

// =============================================================================
// POSITIVE — controls that must remain accepted.
// =============================================================================

// CONTROL: local invocation of a closure capturing a plain borrow param is
// not an escape — the closure's lifetime is bounded by the method body, so
// the source `w` is still live for every call.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           local_invocation_capturing_plain_param_clean) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method Greet(w: Widget) "
        "  var f := lambda() => w.Greet(); "
        "  f(); "
        "end Greet;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a closure capturing NOTHING is returned freely — unaffected by
// this fix.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           return_non_capturing_lambda_clean) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method Make(): () -> () "
        "  return lambda() => Console.WriteLine(\"const\"); "
        "end Make;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: capturing a plain VALUE-typed parameter (int) is not a borrow —
// no source set — so returning the closure is legal.
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           return_lambda_capturing_value_param_clean) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method MakeAdder(n: int): () -> int "
        "  return lambda() => n; "
        "end MakeAdder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a `ref`/`out` parameter capture is already rejected by the
// existing #1217 leg (a different diagnostic) — confirm this fix's new leg
// does not change that outcome (still rejected; the ref/out leg fires
// first).
BEZEL_TEST(plain_borrow_param_closure_escape_4709,
           return_lambda_capturing_ref_param_still_uses_1217_leg) {
    auto r = parseAndCheckOwnership(wrapMethod(
        "public method Make(ref w: Widget): () -> () "
        "  return lambda() => w.Greet(); "
        "end Make;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "#1217"));
}
