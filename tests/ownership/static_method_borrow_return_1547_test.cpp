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

using bezel::test::hasErrorMessage;
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
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a static method may not re-expose static storage") ||
        hasErrorMessage(r, "without `owned`"));
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
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned from a static method"));
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
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned from a static method"));
}

// The rule also covers a FIELD read reached through a parameter (`h.Ref`,
// a non-owned field) — the field's source is `h`, itself a borrowed
// parameter with no receiver-rooted proof of outliving the call.
// The fixture declares `Ref` as an explicit public field: it used to be
// `class Holder(Ref: Res)` with no `end Holder;`, which nested `P` inside
// `Holder`. Once #4868 (§22.10, v0.307) stopped promoting primary-constructor
// parameters, the `h.Ref` read failed as "no member", before this rule ran.
BEZEL_TEST(static_borrow_return_1547, field_through_param_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public Ref: Res; "
        "  public constructor(r: Res) Ref := r; end constructor; "
        "end Holder; "
        "class P "
        "  public static method GetField(h: Holder): Res => h.Ref; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow may not be returned from a static method"));
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

// v0.196 RETIREMENT (#1815): a static method returning a bare read of its own
// STATIC field is a static-storage RE-EXPOSURE, and v0.196 retires the static-
// METHOD arm the v0.187 admission opened (§28.3 ~L5898 "A static **method** may
// not return such a read (v0.196 — retiring the static-*method* arm)"; §28.6
// ~L6085 same). This shape previously pinned that now-retired acceptance;
// flipping it to expect an error IS the spec-mandated behavior change. Re-
// exposure is now the static PROPERTY getter only — see
// `static_property_read_stays_accepted` below, which survives unchanged.
BEZEL_TEST(static_borrow_return_1547, static_field_read_stays_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "  public static method Get(): Res => _shared; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a static method may not re-expose static storage") ||
        hasErrorMessage(r, "without `owned`"));
}

// §28.3 producers table (v0.180), #1546 conformance batch: a STATIC PROPERTY
// itself — "Static property access `T.P` (class-typed result) | Borrowed
// (source: the declaring type's static storage — process-lived, so every
// intra-program liveness constraint is satisfied; static state is disposed
// only at program exit, §22.13)" — stays accepted under v0.201's declaration-
// site static-return error: static properties are explicitly OUTSIDE the
// rule (proposal 0048 §3 — "Static property getters — outside, deliberately.
// They were never under the heuristic … the static property is thus the
// sanctioned complement of this error — the diagnostic's second remedy —
// not a target of it."). v0.201 (#2048) DID retire the body-side admission a
// static METHOD enjoyed when its body returned a static-property read: the
// signature `static method Get(): Res` is now rejected at the declaration
// regardless of body content (the predicate is signature-keyed), so the
// former `Get(): Res => Holder.Shared` shape no longer compiles. The static
// property alone — the still-relevant claim under v0.201 — is what this test
// now pins; the wrapping static method was the v0.196 admission that v0.201
// retires.
BEZEL_TEST(static_borrow_return_1547, static_property_stays_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  private static readonly _shared: Res := new Res(); "
        "  public static property Shared: Res => _shared; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// vNEXT, #1819 (ruling 2026-07-24, spec v0.220→vNEXT): the §28.3 *Borrow
// lifetime rules* / §28.6 *Leak prevention* bullets widen the static-property
// callee-side admission from "a bare read of a `static` `owned` field" to
// "a bare read of a `static` class- or interface-typed field, in any
// admissible form" — `owned`, `readonly` without `owned` (§22.13 exempts the
// referent from ownership-discharge; the value lives for the duration of
// the program), or a §22.12-rule-2 admitted mutable static (whose stores
// never dispose a displaced referent, so every referent it has ever held
// stays alive to process exit). The widening aligns the spec's letter with
// the implementation, the stdlib §3.2 `Encoding` singleton surface, and the
// v0.196 static-method diagnostic's own "re-expose the field through a
// static property" remedy. **This test pins the OWNED form** — the form the
// pre-vNEXT letter covered — so a future narrowing cannot regress it. The
// non-owned and mutable-form siblings below cover the new admissions.
BEZEL_TEST(static_borrow_return_1547, static_property_owned_stays_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  private static owned readonly _shared: Res := new Res(); "
        "  public static property Shared: Res => _shared; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// vNEXT, #1819 (ruling 2026-07-24, spec v0.220→vNEXT): the §28.3/§28.6
// static-property callee-side admission widens to admit a §22.12-rule-2
// ADMITTED MUTABLE STATIC behind a getter. This test pins the form via
// `Mutex` (the form-(i) internally-synchronized class-typed handle from
// stdlib §6) — a mutable static field whose every store is single-pointer-
// word atomic, never disposes a displaced referent, and is exempt from the
// §22.12 rule 4 domain-reachability warning by virtue of being in the
// form-(i) set. The getter re-exposes the storage and the static-property
// call site classifies the result as the §28.3 *Static property access* row
// Borrowed (always-live source, K2). The `T.f` sibling below tests the
// direct (non-getter) read of the same kind of field.
BEZEL_TEST(static_borrow_return_1547, static_property_mutex_stays_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public static _m: Mutex := new Mutex(); "
        "  public static property M: Mutex => _m; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var m: Mutex := Holder.M; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// vNEXT, #1819 (ruling 2026-07-24, spec v0.220→vNEXT): the new
// **Static field access `T.f`** producers row — the caller-side twin of
// the existing `T.P` row, listed alongside it in §28.3 *Producers* — pins
// a direct read of a `static` class- or interface-typed field as Borrowed
// (source: the declaring type's static storage — K2's always-live source).
// The bare in-scope spelling `_shared` (the declaring type's own static
// field) is the §28.3 producers row's other form, equally admitted. This
// test pins the bare-spelling accept so a future narrowing cannot regress
// the direct-read path the encoding-style access `T.f` covers.
BEZEL_TEST(static_borrow_return_1547, static_field_read_in_scope_stays_accepted) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var x: Res := Holder._shared; "
        "    return 0; "
        "  end Main; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.3 v0.201 (#2048): the body-side admission retired — a static method
// whose body returns a static-property READ is now rejected AT THE
// DECLARATION (signature-keyed, body-independent). The v0.196 admission
// opened this for the static-method arm; v0.201 retires it: the
// `static method Get(): Res` signature fails the announced-set rule
// regardless of body. The diagnostic teaches the fix — re-expose via the
// static property directly (which is what `static_property_stays_accepted`
// above exercises), or add `owned` if the body actually produces a fresh
// value.
BEZEL_TEST(static_borrow_return_1547, static_method_wrapping_static_property_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  private static readonly _shared: Res := new Res(); "
        "  public static property Shared: Res => _shared; "
        "  public static method Get(): Res => Holder.Shared; "
        "end Holder;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a static method may not re-expose static storage") ||
        hasErrorMessage(r, "without `owned`"));
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
