// Spec-derived tests for the #1499 fix — the SEMA (ownership-pass) side of
// classifying an unannotated STATIC factory's FRESH-owned return.
//
// §28.3 "Factory methods and sourceless returns" (v0.201, #2048): the v0.60
// treated-owned-with-a-warning heuristic is RETIRED; a static method's
// non-`owned` class/interface return is now a declaration-site ERROR. The
// caller-side classification of an `owned`-returning static factory is
// unchanged: an `owned Res`-returning static call is owned by the caller, and
// the caller must discharge the ownership obligation (store in an `owned`
// field, bind with `using var`, transfer, or consume as an unbound owned
// temporary).
//
// Producers table (§28.3, v0.201): "Method call returning T (no owned) |
// Borrowed — instance method: source is the receiver; extension method:
// source is the receiver parameter. A **static** callee never reaches this
// row: a static method's non-`owned` class/interface-typed return type is
// admissible only where the declared type itself announces the
// classification — a `Task`-family type, an iterable/enumerator interface,
// or a bare type parameter with a non-empty source set — and any other such
// static signature is rejected at its declaration."
//
// Appendix C.3 (v0.201): "A `static` method or operator whose declared
// return type (nullability stripped) is a class or interface type must
// announce its classification in the signature — `owned`, a `Task`-family
// type, a §29.3/§30.7 iterable or enumerator interface, or a bare type
// parameter with a non-empty potential source set; any other such static
// signature (including an empty-set bare-type-parameter return) is a compile
// error at the declaration (§28.3)."
//
// CRITICAL soundness boundary (§28.3 borrow-lifetime rules / "Factory methods"
// non-normative note; the #1499 regression): a static method's body may not
// re-expose static storage through a non-`owned` return — every caller would
// mint a second owner for the borrowed-out storage (v0.196 retired the
// static-METHOD arm of the static-storage admission; the static-property
// getter is the only admitted re-exposure face). The CALLER side of this
// boundary is the v0.201 declaration-site error: a static method returning a
// class type with no `owned` marker fails at the declaration regardless of
// body content.
//
// MIGRATION (v0.201, mechanical): the affected `Make(): Res` signatures below
// are spelled `Make(): owned Res` — the one-token edit the v0.60 warning has
// recommended at every affected declaration. The caller-side classification
// is unchanged (the retired heuristic and the explicit marker both yield the
// identical caller-side Owned state); only the spelling differs.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

// --- Owned-factory half: a FRESH static return is classified Owned. ---

// The issue's exact repro shape: a plain `var` bound to a fresh-owned static
// factory result, never discharged. Before the fix: classified Borrowed, zero
// diagnostics, a silent leak. After: the value is tracked Owned, so the §28.3
// affine auto-drop must-use warning fires (no error — auto-drop is a warning,
// not the discharge error). (v0.201: the factory's signature is now spelled
// `Make(): owned Res` — the explicit marker the declaration-site error
// requires; the caller-side classification is the same as the retired
// heuristic's treated-owned.)
BEZEL_TEST(sourceless_static_factory_1499, plain_var_fresh_autodrops_with_warning) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): owned Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "m"));
}

// Explicit `using var` discharge: no errors, and the must-use warning is
// suppressed (the scope-bound intent is explicit — §28.3 "Must-use warning").
// (v0.201: `Make(): owned Res` per the declaration-site error's remedy.)
BEZEL_TEST(sourceless_static_factory_1499, fresh_using_var_discharges_no_warning) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): owned Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    using var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "m"));
}

// The value is genuinely OWNED, not merely warned-about: it can be transferred
// out through an `owned` return, exactly like a `new` producer (Appendix C.3:
// "the same discharge rules as `new` apply"). (v0.201: `Make(): owned Res` per
// the declaration-site error's remedy.)
BEZEL_TEST(sourceless_static_factory_1499, fresh_owned_return_transfer_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): owned Res return new Res(); end Make; "
        "  public static method Relay(): owned Res return Make(); end Relay; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Borrow-return half: the SEMA REGRESSION GUARD. ---

// v0.196 RETIREMENT (#1815): a static method whose body returns a static-field
// READ (`_shared`) is a static-storage RE-EXPOSURE. v0.187 admitted this from a
// static method alongside a static property getter; v0.196 retires the static-
// METHOD arm (§28.3 ~L5898 "A static **method** may not return such a read";
// §28.6 ~L6085 "a static **method** returning such a read is rejected"): every
// caller classifies the call as the sourceless-factory case and would mint a
// second owner for the borrowed-out storage. This test previously pinned the
// now-retired acceptance; flipping it to expect an error IS the spec-mandated
// behavior change. (The §22.12-rule-4 domain warning on the static readonly
// field is a separate Warning; the borrow-return Error is what makes hasErrors
// true. `Get()` still returns a borrow, so its call site does not auto-drop.)
BEZEL_TEST(sourceless_static_factory_1499, borrow_field_return_stays_borrowed) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "  public static method Get(): Res => _shared; "
        "  public static method Main(): int "
        "    var r := Get(); "
        "    return 0; "
        "  end Main; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "r"));
}

// §28.3 / §28.6 (v0.196, #1815) — the retirement's own FINGERPRINT: the exact
// v0.187-admitted-then-v0.196-retired shape, a `static owned` field re-exposed
// through a static method returning a bare read of it. (The field is spelled
// `static readonly owned` — a *mutable* static of class type is rejected upstream
// by §22.12 rule 2, so the realizable "static-owned" shape the issue names is the
// readonly one.) Both the `=>`-expression and block-`return` method bodies must
// reject, and the diagnostic must name the static-PROPERTY remedy so migration is
// mechanical.
namespace {
// True when some ERROR diagnostic names the static-property re-exposure remedy.
bool hasStaticPropertyRemedyError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("static property") != std::string::npos) return true;
    }
    return false;
}
}  // namespace

BEZEL_TEST(sourceless_static_factory_1499, static_owned_field_reexposed_by_method_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Config "
        "  public static owned readonly _instance: Config := new Config(); "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public static method Instance(): Config => _instance; "
        "end Config;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticPropertyRemedyError(r));
}

// Sibling block-body form (`return _instance;`) — the full-body dataflow path
// (handleReturn) must reject it identically to the `=>` expression-body path
// (runExprBodyOwningReturnCheck) above.
BEZEL_TEST(sourceless_static_factory_1499, static_owned_field_reexposed_by_method_blockbody_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Config "
        "  public static owned readonly _instance: Config := new Config(); "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "  public static method Instance(): Config return _instance; end Instance; "
        "end Config;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticPropertyRemedyError(r));
}

// Negative control: the rule is STATIC-only. An INSTANCE method's non-`owned`
// class return has a structural source (the receiver) and stays Borrowed — its
// call-site result must NOT be flagged for auto-drop (§28.3 producers row).
BEZEL_TEST(sourceless_static_factory_1499, instance_method_receiver_source_stays_borrowed) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public owned _r: Res; "
        "  public constructor(owned r: Res) _r := r; end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Res => _r; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(new Res()); "
        "    var b := h.Get(); "
        "    h.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}
