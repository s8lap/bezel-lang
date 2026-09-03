// Spec-derived tests for issue #1973 — §22.5 named-argument binding must drive
// the §28.8 owned-sink argument classification (both directions).
//
// Normative text:
//   §22.5 (v0.184): "positional arguments fill parameters in order; a named
//     argument `name := expr` fills the parameter named `name`; named arguments
//     may appear in any order after positional arguments."
//   §28.8 (v0.184): a plain `owned` parameter is a CONSUMING sink — the argument
//     is transferred into the callee and left moved-from; a borrow parameter is
//     not. §22.3 (v0.169): the consuming-ness of an argument is determined by the
//     BOUND parameter's `owned` modifier.
//
// The bug (#1973): the ownership layer's owned-sink classification indexed the
// parameter list by the argument's SOURCE position, ignoring the `name := value`
// binding. When a named argument REORDERS an owned/borrow pair, both error
// directions occurred:
//   * OVER-REJECT — a sound borrow is rejected. `Sink2(owned a, b)` called
//     `Sink2(b := r, a := new R())` with `r` a borrow: `r` binds the BORROW
//     parameter `b` but sits at source index 0 (the owned slot), so it was
//     wrongly reported "a borrow cannot be passed where an `owned` value is
//     required".
//   * MISS → UAF — a consume is not detected. `Sink3(a, owned b)` called
//     `Sink3(b := r, a := 7)` with `r` an owned local: `r` binds the OWNED
//     parameter `b` at source index 0 (the borrow slot), so the consume was
//     missed — `r` stayed Owned, the callee disposed it, and the caller then
//     re-used / re-disposed the freed object (double-free / heap use-after-free;
//     confirmed: the pre-fix repro compiled rc=0 and SEGV'd under ASan).
//
// The fix (Ownership_Checks.cpp, boundParamIndex): every owned-sink argument
// classification — recordMovesInArgs (method call), the `new T(...)` ctor arms,
// isImplicitOwned (bare-statement call), and calleeArgIsOwnedSink (the #1958
// closure pass) — resolves each argument to its BOUND parameter via §22.5
// binding before checking `params[boundPos].modifier == "owned"`, so the closure
// pass and the top-level passes cannot diverge.
//
// Note: the `owned` call-site marker is POSITIONAL-ONLY (§22.3 / A.7 — a named
// argument is `name := Expression`, and `owned r` is not an expression). So once
// the consume is recognized, a named-argument transfer of an owned binding can
// never carry the required marker: it is correctly rejected with the §22.3
// "write `owned r`" diagnostic, steering the transfer to positional form
// (`Sink3(7, owned r)`, exercised by the positive control). That is the sound
// outcome — the alternative (silently accepting the consume) is the UAF.
//
// Fail-without / pass-with:
//   * over-reject:  WITHOUT the fix the borrow-reorder call is REJECTED (borrow
//     → owned); WITH the fix it compiles clean.
//   * miss:         WITHOUT the fix the owned-reorder call compiles with NO
//     consume diagnostic (→ runtime UAF); WITH the fix the consume is detected
//     ("this call consumes `r`", §22.3).

// AMENDED (fix #3746 follow-up, v0.286): the shared prelude's `Holder` field is
// now `public owned _b: R;`. As originally written (`public _b: R;`) the ctor
// stored a borrow of its own `owned` parameter into a non-owning field and the
// parameter was auto-dropped at the constructor's end — a dangling `_b` for the
// object's whole life. #3746 made that store an error, and the four "accepted"
// rows below (which assert `!hasErrors()` over this shared prelude) went red on
// the gate. The repair is the §28.8 transfer, not a narrowing of #3746: no
// narrowing exists that accepts this prelude and still rejects the four
// measured use-after-frees #3746's own rows pin, since `Box.Take(p: owned R)
// { this.r := p; }` and this constructor are the same program. See the comment
// at the field itself for the measurement.

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::Severity;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// R is a genuine non-trivial-dispose resource (its double-dispose is the
// soundness hazard). Sink2 has the OWNED parameter FIRST (`owned a`, then borrow
// `b`); Sink3 has it SECOND (borrow `a: int`, then `owned b`) — the two reorder
// directions. Peek2 is all-borrow. Holder's constructor mirrors Sink3's shape so
// the `new T(...)` owned-sink arm is exercised too.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public method Get(): int return V; end Get; "
        "  destructor end destructor; "
        "end R; "
        // fix #3746 follow-up: `_b` is declared `owned`. It was a plain
        // `public _b: R;`, which made this prelude an instance of exactly the
        // §28.3 defect #3746 closed: an `owned` ctor parameter stored into a
        // NON-owned field is a borrow store, the parameter is then auto-dropped
        // at the constructor's end (the compiler's own pre-existing diagnostic
        // says so verbatim — "owned parameter `b` is implicitly disposed at the
        // end of the method (§28.3 auto-drop)"), and `_b` dangles for the whole
        // life of the Holder, with no site-announced kill anywhere that §28.2's
        // backstop could key on. MEASURED on a31fb7b: the identical shape with a
        // caller-side `Dispose()` (the only spelling the pre-#3746 compiler
        // accepted) prints a garbage int in unchecked mode and raises
        // `System.DisposedAccessException` under `--checked-disposal`.
        // `owned _b` is the §28.8 transfer — the ctor parameter is DISCHARGED
        // into the field, the Holder's destructor owns it, and the auto-drop
        // warning disappears. Nothing this file tests changes: `Holder`'s
        // constructor keeps its `(a: int, owned b: R)` signature, which is the
        // only thing the §22.5 named-argument binding rows below exercise.
        "class Holder "
        "  public owned _b: R; "
        "  public constructor(a: int, owned b: R) this._b := b; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Sink2(owned a: R, b: R): int "
        "    a.Dispose(); return b.V; "
        "  end Sink2; "
        "  public static method Sink3(a: int, owned b: R): int "
        "    b.Dispose(); return a; "
        "  end Sink3; "
        "  public static method Peek2(a: int, b: R): int "
        "    return b.V + a; "
        "  end Peek2; "
        ) + body + " end C;";
}

// True iff some error diagnostic contains `needle`.
bool errorSays(const OwnershipResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The §28.3 v0.184 captured-owned-consume error (the #1958 closure diagnostic).
bool rejectsCapturedOwnedConsume(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Error) continue;
        if (d.message.find("captured owned binding") != std::string::npos &&
            d.message.find("§28.3, v0.184") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// --- Direction 1: OVER-REJECT of a sound borrow (now ACCEPTED) --------------

// §22.5 / §28.8: `Sink2(owned a, b)` called `Sink2(b := r, a := new R())` with
// `r` a BORROW parameter. `r` binds the borrow parameter `b`; the fresh producer
// `new R()` binds the owned parameter `a`. This is sound — no owned binding is
// consumed — and must compile. WITHOUT the fix `r` was mapped to the owned slot
// by its source index (0) and rejected "a borrow cannot be passed where an
// `owned` value is required". This is the load-bearing over-reject case.
BEZEL_TEST(named_arg_owned_sink_1973, borrow_reorder_to_borrow_param_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Use(r: R): int "
        "  return C.Sink2(b := r, a := new R()); "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (positional classification unchanged): the SAME borrow `r` passed
// POSITIONALLY into the owned slot (`Sink2(r, new R())`, `r` at index 0 → owned
// `a`) is still correctly rejected — a borrow may not flow into an owned sink.
// Guards that the boundParamIndex refactor left positional classification exact.
BEZEL_TEST(named_arg_owned_sink_1973, borrow_positional_to_owned_param_still_rejected_28_8) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Use(r: R): int "
        "  return C.Sink2(r, new R()); "
        "end Use;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "a borrow cannot be passed where an `owned`"));
}

// --- Direction 2: MISS of a consume → UAF (now DETECTED) --------------------

// §22.5 / §28.8: `Sink3(a: int, owned b)` called `Sink3(b := r, a := 7)` with
// `r` an OWNED local. `r` binds the OWNED parameter `b` — the call CONSUMES it.
// WITHOUT the fix `r` was mapped to the borrow slot (`a`) by its source index
// (0), the consume was missed, `r` stayed Owned, and the later use double-freed
// (confirmed SEGV under ASan). WITH the fix the consume is detected: §22.3
// requires the transfer be announced ("this call consumes `r`"). The marker is
// positional-only, so the named form is thereby rejected — the sound outcome.
BEZEL_TEST(named_arg_owned_sink_1973, owned_reorder_to_owned_param_consume_detected_22_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var r := new R(); "
        "  var n := C.Sink3(b := r, a := 7); "
        "  r.Dispose(); "
        "  return n; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `r`"));
}

// The MISS direction at a bare-STATEMENT call (`Sink3(b := r, a := 7);`, no
// enclosing LocalDecl/assignment) — the handleDischarge / isImplicitOwned path,
// a distinct owned-sink classification site that shared the same blindness. The
// consume must be detected here too, or that path diverges from recordMovesInArgs.
BEZEL_TEST(named_arg_owned_sink_1973, owned_reorder_bare_statement_consume_detected_22_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var r := new R(); "
        "  C.Sink3(b := r, a := 7); "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `r`"));
}

// The MISS direction at a CONSTRUCTOR call (`new Holder(b := r, a := 7)`,
// `Holder(a: int, owned b)`) — the NewExpr owned-sink arm, another site with the
// same blindness. WITHOUT the fix `r` mapped to the int slot and the consume was
// missed (double-free: Holder's auto-disposer + the caller's `r`). WITH the fix
// the consume of `r` into the owned ctor parameter `b` is detected.
BEZEL_TEST(named_arg_owned_sink_1973, owned_reorder_to_owned_ctor_param_consume_detected_22_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var r := new R(); "
        "  using var h := new Holder(b := r, a := 7); "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(errorSays(r, "this call consumes `r`"));
}

// --- Closure pass (#1958 calleeArgIsOwnedSink) must honor the reorder too ----

// §28.3 v0.184: consuming a captured owned binding inside a closure is an error.
// The consume here is a REORDERED named argument (`Sink3(b := r, a := 7)`, `r`
// captured, bound to owned `b`). WITHOUT the fix calleeArgIsOwnedSink mapped `r`
// to the int slot and missed it — the closure would double-dispose at runtime,
// and the closure check would be MORE lenient than the (also-buggy) top level.
// WITH the fix the closure pass binds by name identically and rejects it.
BEZEL_TEST(named_arg_owned_sink_1973, closure_owned_reorder_consume_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return C.Sink3(b := r, a := 7); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// CONTROL (closure false-positive guard): the same captured owned binding `r`
// passed by a REORDERED named argument into a BORROW parameter (`Peek2(b := r,
// a := 7)`, `b` a plain `R`) is a legitimate borrow-in-closure and must compile.
// This is the closure twin of the over-reject direction: the fix must accept it,
// never mis-flag it as a captured-owned consume.
BEZEL_TEST(named_arg_owned_sink_1973, closure_borrow_reorder_to_borrow_param_accepted_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return C.Peek2(b := r, a := 7); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Positive controls: ordinary spellings still classified correctly -------

// CONTROL: the legitimate way to consume `r` into the owned parameter is
// POSITIONAL with the marker (`Sink3(7, owned r)`) — the marker is positional-
// only. `r` is consumed exactly once and the call compiles clean. Guards that
// the fix does not block a sound positional owned transfer.
BEZEL_TEST(named_arg_owned_sink_1973, positional_owned_transfer_with_marker_accepted_22_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var r := new R(); "
        "  return C.Sink3(7, owned r); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: an all-named call that does NOT reorder — names in declaration order
// (`Sink2(a := new R(), b := r)`, `r` a borrow) — is the trivial named-arg case
// and must compile: `a` gets the fresh producer (owned), `b` gets the borrow.
// Guards that named args in declaration order still map to their own positions.
BEZEL_TEST(named_arg_owned_sink_1973, named_in_declaration_order_accepted_22_5) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Use(r: R): int "
        "  return C.Sink2(a := new R(), b := r); "
        "end Use;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
