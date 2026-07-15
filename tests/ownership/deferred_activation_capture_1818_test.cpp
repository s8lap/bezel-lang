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
// T2 covers the FIRST provenance row only: a LAMBDA LITERAL argument. Its
// captures are reduced EXACTLY as §28.14's storage-bounded admission
// (checkScopedAdmission) reduces them — `this`/receiver-members and statics and
// value-typed locals contribute nothing — and a captured LIVE OWNED obligation
// of the enclosing frame (an owned local / `using var`, or an `owned`
// parameter) is flagged when the frame ESCAPES (bound to a name, returned, or
// stored — not a direct `for`/`await for` source, whose desugared `using var`
// bounds the frame by the loop, which every captured method-scope source
// outlives). Method references, opaque function-typed parameters, and traced
// locals are later #1818 provenance rows, out of scope here.
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
