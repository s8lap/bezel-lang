// Spec-derived tests for #1818 T2 (§28.3 "capture-set contribution", §28.14
// "Call-bounded × deferred activation" frame residence, v0.196).
//
// §28.3 (v0.196): the returned deferred-activation frame "is treated as a
// borrow-carrying value whose sources are the call's receiver, its plain
// reference-typed arguments, and the **capture-set contribution** of each plain
// function-typed argument ... a plain function-typed argument contributes its
// captured borrows' **sources** to the frame's source set." And §28.3
// "Returning a deferred-activation frame": "A returned frame whose capture-set
// contribution reaches a scope-bound local, a `using var`, or a temporary is
// rejected ... `return Doubled(nums, (x) => scale.Apply(x));` over a `using var
// scale` is the canonical reject: the consumer would enumerate into a closure
// over a disposed object." "A contribution that is the method's own plain
// function-typed parameter, or a capture sourced from its parameters or
// receiver, is admitted."
//
// T2 covers the FIRST provenance row: a LAMBDA LITERAL argument. Its captures
// are reduced EXACTLY as §28.14's storage-bounded admission
// (checkScopedAdmission) reduces them — `this`/receiver-members and statics and
// value-typed locals contribute nothing — and a captured LIVE OWNED obligation
// of the enclosing frame (an owned local / `using var`, or an `owned`
// parameter) is flagged when the frame ESCAPES (bound to a name, returned, or
// stored — not a direct `for`/`await for` source, whose desugared `using var`
// bounds the frame by the loop, which every captured method-scope source
// outlives).
//
// T3 adds the METHOD-REFERENCE provenance row (§28.3 provenance table, §27.6,
// v0.196): a method reference `obj.M` passed into a plain function slot carries
// its RECEIVER as the captured source — reduced to the receiver's root sources
// EXACTLY as the lambda captures are. A STATIC method reference (`Program.Helper`)
// carries no owned receiver; an instance method reference whose receiver is a
// LIVE OWNED obligation and whose frame escapes is rejected, mirroring the
// lambda `(x) => scale.Apply(x)` twin. Opaque function-typed parameters, traced
// locals, and plain-storage (function-typed field) fetches are later #1818
// provenance rows, out of scope here.
//
// Every shape below was hand-verified against `build/bezelc -c` on a standalone
// probe before being folded in (the REJECT shapes compile CLEAN without the T2
// fix — the capture hole — and produce the capture-set-contribution error with
// it; the ADMIT shapes compile clean either way).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Preamble shared by every test:
//  - Scaler: a disposable class (destructor) with borrowing methods `Apply` /
//    `Test`, used both as the captured owned `using var` and as a borrow
//    source (parameter / local).
//  - Program.Doubled(nums, f): IEnumerable<int> — the spec's `Doubled` shape,
//    a USER synchronous iterator factory taking a PLAIN function-typed
//    parameter `f` (frame-residence position, §28.14 v0.196). Called from an
//    instance method so `this` / a static field are available as capture
//    sources that must contribute NOTHING.
//  - Program.Scale / Program.Factor — an instance method and a static field,
//    the `this`-mediated and static capture sources that contribute nothing.
//  - Program.Helper — a STATIC `(int) -> int` method, referenced as
//    `Program.Helper` (a method reference with a type-name receiver that carries
//    no owned obligation — the T3 static-reference admit control).
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

// True when a capture-set-contribution diagnostic (the #1818 T2 error) is
// present — pins THIS bug rather than any incidental error.
bool sawCaptureSetError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("capture-set contribution") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// REJECT — a captured LIVE OWNED obligation reached through a lambda argument
// whose deferred-activation frame ESCAPES.
// ===========================================================================

// ★ Primary regression (design test iii — non-return binding position). The
// lambda `(x) => scale.Apply(x)` captures the owned `using var scale`; the
// frame is bound to `q` and `q` is returned, so the frame escapes `scale`'s
// scope. Rejected. Compiles CLEAN without the T2 fix (the capture hole);
// produces the capture-set-contribution error with it. // per §28.3, §28.14
BEZEL_TEST(deferred_activation_capture_1818, bind_then_return_owned_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "var q := Doubled(nums, (x) => scale.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The for-in-over-`q` variant (design test iv): the frame is bound to `q`, then
// enumerated by a loop over `q`. Rejected at the binding, exactly as the
// return variant — a capturing frame bound to a name is a conservative escape.
// // per §28.3, §28.14
BEZEL_TEST(deferred_activation_capture_1818, bind_then_forin_owned_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "var q := Doubled(nums, (x) => scale.Apply(x)); "
        "var total := 0; "
        "for var v in q do total := total + v; end; "
        "return total;",
        "method M(nums: Array<int>): int"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The spec's canonical reject, verbatim: `return Doubled(nums, (x) =>
// scale.Apply(x));` directly returning the frame over a `using var scale`.
// // per §28.3 "Returning a deferred-activation frame"
BEZEL_TEST(deferred_activation_capture_1818, direct_return_owned_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "return Doubled(nums, (x) => scale.Apply(x));",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// An `owned` PARAMETER is a live owned obligation the method disposes at exit
// (§28.3 auto-drop). A lambda capturing it, whose frame escapes (returned),
// would dangle after that disposal — rejected. (The owned param also draws the
// ordinary must-use auto-drop WARNING, but the capture-set error is the
// load-bearing diagnostic pinned here.) // per §28.3, §28.14
BEZEL_TEST(deferred_activation_capture_1818, owned_param_capture_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, (x) => s.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>, owned s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// ===========================================================================
// ADMIT — the reduction's exemptions and the non-escaping positions. Each must
// compile clean (guards the over-rejection risk the T2 reduction targets).
// ===========================================================================

// The lambda captures ONLY `this` (via `this.Scale`), a value-typed local
// (`bump`), and a static field (`Program.Factor`) — none a borrow of an owned
// obligation, so the capture-set contribution is empty and the frame carries no
// scope-bound borrow. Admitted even though it is bound to `q` and returned.
// This pins checkScopedAdmission's exact reductions: `this`/receiver-member →
// nothing, static ref → nothing, value-typed capture → nothing. // per §28.14
BEZEL_TEST(deferred_activation_capture_1818, capture_nothing_owned_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var bump: int := 5; "
        "var q := Doubled(nums, (x) => this.Scale(x) + bump + Program.Factor); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The frame is NOT escaped: it is used directly as a `for` source and dropped.
// The `for` desugaring's `using var` bounds the frame by the loop, which the
// captured `using var scale` (declared in the enclosing method scope)
// outlives — sound, admitted. The T2 check deliberately excludes this
// for-source position (unlike the #1526 unbound-temporary rule, whose
// temporary source dies before the loop runs). // per §28.3, §29.2, §28.14
BEZEL_TEST(deferred_activation_capture_1818, direct_for_source_not_escaped_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "var total := 0; "
        "for var v in Doubled(nums, (x) => scale.Apply(x)) do "
        "  total := total + v; "
        "end; "
        "return total;",
        "method M(nums: Array<int>): int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A capture of a plain (borrow) PARAMETER is admitted: the parameter outlives
// the method, and the caller re-establishes the returned frame's constraint
// against its own actual argument (§28.3's parameter-sourced admission). Guards
// against over-rejecting the spec's admitted parameter-sourced case. // per §28.3
BEZEL_TEST(deferred_activation_capture_1818, borrow_param_capture_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, (x) => s.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>, s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A capture of a BORROW local (`var b := s;`, aliasing a borrow parameter) is a
// carried source but NOT a live owned obligation — its real owner outlives the
// enclosing frame — so it is admitted here (its transitive re-establishment is
// a later #1818 provenance row). Guards the owned-vs-borrow local distinction.
// // per §28.3, §28.14
BEZEL_TEST(deferred_activation_capture_1818, borrow_local_capture_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var b := s; "
        "var q := Doubled(nums, (x) => b.Apply(x)); "
        "return q;",
        "method M(nums: Array<int>, s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// T3 — METHOD-REFERENCE provenance row (§28.3 provenance table, §27.6, v0.196).
// A method reference `obj.M` carries its RECEIVER as the captured source; the
// receiver is reduced to its root sources exactly as a lambda's captures are.
// Each REJECT below compiles CLEAN without the T3 fix (the method-reference
// capture hole — a method reference is a MemberAccessExpr, not a LambdaExpr, so
// the T2 dispatch missed it) and produces the capture-set-contribution error
// with it; each ADMIT compiles clean either way. Every shape was hand-verified
// against `build/bezelc -c` on a standalone probe before being folded in.
// ===========================================================================

// ★ Primary T3 regression: the spec's canonical reject with the LAMBDA replaced
// by its method-reference twin — `return Doubled(nums, scale.Apply);` directly
// returns the frame over a `using var scale`. The method reference closes over
// the owned receiver `scale`; returning the frame strands that borrow past
// `scale`'s disposal. Rejected. // per §28.3 provenance method-reference row, §27.6, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_direct_return_owned_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "return Doubled(nums, scale.Apply);",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The bind-then-return variant: the frame is bound to `q`, then `q` is returned.
// Rejected at the binding, exactly as the lambda twin. // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_bind_then_return_owned_local_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "var q := Doubled(nums, scale.Apply); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The receiver of the method reference is an `owned` PARAMETER — a live owned
// obligation the method disposes at exit (§28.3 auto-drop). The escaping frame
// would dangle after that disposal; rejected. (The owned param also draws the
// must-use auto-drop warning; the capture-set error is the load-bearing
// diagnostic pinned here.) // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_owned_param_receiver_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, s.Apply); "
        "return q;",
        "method M(nums: Array<int>, owned s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// ADMIT — a STATIC method reference (`Program.Helper`) carries no receiver: its
// type-name root reduces to nothing, so the frame carries no owned obligation.
// Admitted even though the frame is returned. Pins the "static reference
// contributes nothing" reduction. // per §28.3, §27.6, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_static_reference_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "return Doubled(nums, Program.Helper);",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT — the method reference's receiver is a plain (borrow) PARAMETER, which
// outlives the method; the caller re-establishes the returned frame's constraint
// against its own actual argument (§28.3's parameter-sourced admission). Guards
// against over-rejecting a borrow-receiver method reference. // per §28.3, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_borrow_param_receiver_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, s.Apply); "
        "return q;",
        "method M(nums: Array<int>, s: Scaler): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT — the method reference's receiver is `this` (`this.Scale`). `this` is a
// caller-frame borrow that outlives every local of this frame — it contributes
// nothing (checkScopedAdmission's rule), exactly as a lambda capturing only
// `this` does. Admitted even when returned. // per §28.3, §28.14, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_this_receiver_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, this.Scale); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ADMIT — the frame is NOT escaped: the method reference over the owned `using
// var scale` feeds a direct `for` source, whose desugared `using var` bounds the
// frame by the loop, which `scale` (declared in the enclosing method scope)
// outlives. Sound, admitted — the T3 check excludes the for-source position
// exactly as the T2 lambda check does. // per §28.3, §29.2, §28.14, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_direct_for_source_not_escaped_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "using var scale := new Scaler(); "
        "var total := 0; "
        "for var v in Doubled(nums, scale.Apply) do "
        "  total := total + v; "
        "end; "
        "return total;",
        "method M(nums: Array<int>): int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// T4 — METHOD-REFERENCE over a FRESH PRODUCER receiver (§28.3, v0.196). When the
// method reference's receiver is a fresh producer — a `MemberAccessExpr` whose
// target is a `new`/call producing an UNBOUND OWNED TEMPORARY (`new Scaler().Apply`)
// — the captured source is that temporary, not a named scope-bound local. Per
// §28.3's same-full-expression rule it is admissible ONLY when the returned frame
// is fully consumed within the same full expression (a direct `for` source);
// rejected when the frame is bound/returned/stored. Each REJECT compiles CLEAN
// without the T4 fix (the fresh-receiver hole: `isUnboundProducerExpr` sees the
// MemberAccessExpr wrapper, not its NewExpr/CallExpr target, so the receiver's
// unbound-owned-temporary status was never classified) and produces the
// capture-set-contribution error with it; the ADMIT compiles clean either way.
// Every shape was hand-verified against `build/bezelc -c` before folding in.
// ===========================================================================

// ★ Primary T4 regression: the frame is bound to `q`, then `q` is returned. The
// method reference closes over the fresh owned temporary `new Scaler()`, which
// is disposed at the end of the full expression that produces it; the escaping
// frame would enumerate into a closure over that disposed temporary. Rejected.
// Compiles CLEAN without the T4 fix; produces the capture-set-contribution error
// with it. // per §28.3 capture-set contribution, §27.6, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_fresh_receiver_bind_then_return_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "var q := Doubled(nums, new Scaler().Apply); "
        "return q;",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The direct-return variant: `return Doubled(nums, new Scaler().Apply);` returns
// the frame directly. Same hazard, rejected. // per §28.3, §27.6, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_fresh_receiver_direct_return_rejected) {
    auto r = parseAndCheckOwnership(wrap(
        "return Doubled(nums, new Scaler().Apply);",
        "method M(nums: Array<int>): IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// ADMIT — the same fresh-receiver method reference where the frame is fully
// consumed WITHIN the full expression: a direct `for` source, whose desugared
// `using var` bounds the frame — and its captured temporary — by the loop, which
// lives and dies in the loop header. Sound, admitted. This is the same-full-
// expression admission the §28.3 ruling names, and the over-rejection guard for
// the T4 fix: the fresh-receiver reject must NOT fire when the frame is consumed
// in the expression (the `forSource` position is excluded). // per §28.3, §29.2, v0.196
BEZEL_TEST(deferred_activation_capture_1818, methodref_fresh_receiver_for_source_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "var total := 0; "
        "for var v in Doubled(nums, new Scaler().Apply) do "
        "  total := total + v; "
        "end; "
        "return total;",
        "method M(nums: Array<int>): int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
