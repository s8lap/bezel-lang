// Spec-derived tests for the #1547 fix — a STATIC method returning a
// BORROW through a plain (non-owning) class/interface return type
// (§28.3 "Borrow lifetime rules").
//
// §28.3: "A borrowed local may not be returned from a method at all unless
// the return is a borrow with a clearly-defined source that outlives the
// call — in this version: a receiver-sourced borrow from an instance method
// (a bare owned-field read, `this`, or a borrow reached through the
// receiver's fields and indexers), or the receiver-parameter borrow of an
// extension method. A borrowed local or parameter with no source that
// outlives the call — e.g. a static method returning one of its parameters,
// `static method Pick(a: Res, b: Res): Res return a;` — may not be
// returned; this callee-side prohibition is what makes the caller-side
// sourceless-return owned-assumption (Factory methods) sound."
//
// The rationale changelog (v0.180) names this exact rule "the Pick-
// prohibition/factory-soundness composition note": the "Factory methods"
// section's caller-side rule classifies ANY non-`owned` static-method
// return with no identifiable source as OWNED (with a warning) — the
// compiler does not inspect the callee's body at the call site, so it
// cannot tell "a fresh factory result" from "a re-borrow of one of my own
// arguments." If a static method were allowed to return a borrowed
// parameter (or a local/field derived from one), every call site would
// misclassify that borrow as an owned value and eventually auto-dispose it
// — a double-free of whatever the caller's argument really belongs to.
// Closing that gap requires rejecting EVERY borrow return from a static
// method, not just ones sourced from a local — a static method simply has
// no receiver to serve as the "provably outlives the call" source the
// borrow-lifetime rule requires.
//
// A static method's return stays legal when the returned value is: (a) a
// freshly-produced or `owned`-transferred value (the "Factory methods"
// sourceless-owned-return path, unaffected by this rule); or (b) a read of
// the declaring type's own STATIC storage (a static field or property) —
// process-lived, so it always outlives the call (§28.3 producers table,
// "Static property access ... Borrowed (source: the declaring type's
// static storage — process-lived)"). An INSTANCE method's receiver-sourced
// borrow (`this`, a bare `this`-field read) is a completely separate,
// still-legal borrow-return form and must not regress.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; ") +
        body;
}

}  // namespace

// --- REJECT: a static method returning a borrow with no receiver source. ---

// §28.3 (#1547 repro, spec's own canonical example): `a` is a non-`owned`
// parameter of a STATIC method — a borrow whose source is the caller's
// frame. The static method has no receiver, so there is no admissible
// borrow-returning source; this must be rejected (block-body form).
BEZEL_TEST(static_borrow_return_1547, param_return_blockbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Pick(a: Res, b: Res): Res "
        "    return a; "
        "  end Pick; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Same repro, expression-bodied form (`=>`) — the narrow expression-body
// check (runExprBodyOwningReturnCheck) must reject it identically, not just
// the full block-body dataflow.
BEZEL_TEST(static_borrow_return_1547, param_return_exprbody_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Pick(a: Res, b: Res): Res => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The rule extends to a LOCAL re-borrowed from a parameter, not merely the
// bare parameter identifier — `var b := a;` makes `b` Borrowed (source: the
// same caller frame `a` borrows), and it has exactly the same no-receiver
// problem when returned.
BEZEL_TEST(static_borrow_return_1547, local_reborrow_of_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Relay(a: Res): Res "
        "    var b := a; "
        "    return b; "
        "  end Relay; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The rule also covers a FIELD read reached through a parameter (`h.Ref`,
// a non-owned field) — the field's source is `h`, itself a borrowed
// parameter with no receiver-rooted proof of outliving the call.
BEZEL_TEST(static_borrow_return_1547, field_through_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public Ref: Res := null; "
        "end Holder; "
        "class P "
        "  public static method GetField(h: Holder): Res => h.Ref; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- ACCEPT: legitimate static-method returns are unaffected. ---

// A genuinely FRESH value transferred through an explicit `owned` return
// type is the "Factory methods" path, not a borrow return at all — exempt
// from this rule entirely (returnOwned_ short-circuits the check).
BEZEL_TEST(static_borrow_return_1547, fresh_owned_return_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): owned Res "
        "    return new Res(); "
        "  end Make; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A static method returning its own STATIC field is process-lived storage
// — always outlives the call — and must stay accepted (the #1499
// regression-guard shape; this is the same "static storage" exemption the
// §28.3 producers table states explicitly for a static PROPERTY read).
BEZEL_TEST(static_borrow_return_1547, static_field_read_stays_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "  public static method Get(): Res => _shared; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- ACCEPT: instance-method receiver-sourced returns are a separate,
//     still-legal form and must not regress. ---

// `this` returned from an INSTANCE method is receiver-sourced — the
// caller holds the receiver across the call, so there is no dangling
// source (§28.6 "Methods returning `this`"). This rule is gated on
// isStatic_ and must never fire for an instance method.
BEZEL_TEST(static_borrow_return_1547, instance_method_returns_this_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public method Identity(): Res "
        "    return this; "
        "  end Identity; "
        "end P;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A bare read of an INSTANCE method's own owned field is receiver-sourced
// (source: the receiver) — §28.6's "Methods returning `this`" carve-out
// extended to fields; must stay accepted.
BEZEL_TEST(static_borrow_return_1547, instance_method_returns_owned_field_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _r: Res; "
        "  public constructor(owned r: Res) _r := r; end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Res => _r; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- ACCEPT: the pre-existing out-of-scope boundary (builtin, non-owning
//     collection return types) is unaffected — pinned here as a regression
//     guard for THIS specific change (isBorrowableType requires a
//     user-declared class/interface `declaration`; a builtin collection has
//     none, so it is conservatively out of scope, matching
//     borrow_return_owning_type_1007_test.cpp's array_int_borrow_return_
//     clean). This is not an endorsement that the shape is dangle-free —
//     it documents an existing, accepted limitation this fix does not
//     widen. ---
BEZEL_TEST(static_borrow_return_1547, builtin_collection_param_return_out_of_scope) {
    auto r = parseAndCheckOwnership(
        "module M; class P "
        "  public static method Id(a: Array<int>): Array<int> => a; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
