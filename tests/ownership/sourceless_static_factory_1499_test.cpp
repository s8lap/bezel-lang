// Spec-derived tests for the #1499 fix — the SEMA (ownership-pass) side of
// classifying an unannotated STATIC factory's FRESH-owned return.
//
// §28.3 "Factory methods and sourceless returns": "When a method returns a
// class type T (no `owned`) and the call has no structural borrow source — the
// method is `static` — the return is treated as owned by the caller ... The
// caller must discharge the ownership obligation."
//
// Producers table (§28.3): "Method call returning T (no owned) | Borrowed —
// instance method: source is the receiver; static method: no source, treated
// as owned with a warning (see Factory methods below)."
//
// Appendix C.3: "A static method returning a class type without `owned` and
// without an identifiable borrow source produces an owned value; the same
// discharge rules as `new` apply."
//
// CRITICAL soundness boundary (§28.3 borrow-lifetime rules / "Factory methods"
// non-normative note; the #1499 regression): the caller-side owned-assumption
// holds ONLY for a genuinely FRESH-owned return. A static method returning a
// BORROW — a static-field read (`_shared`, process-lived source, §28.3
// producers "Static property access ... Borrowed") — must stay Borrowed at the
// call site; classifying it owned makes `using var r := Get()` dispose a
// shared/borrowed object (a double-free, §28.10). common::
// staticFactoryReturnsFreshOwned distinguishes the two by requiring EVERY
// return path to be a fresh owned producer.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

// --- Owned-factory half: a FRESH static return is classified Owned. ---

// The issue's exact repro shape: a plain `var` bound to an unannotated static
// factory's `new Res()` result, never discharged. Before the fix: classified
// Borrowed, zero diagnostics, a silent leak. After: the value is tracked Owned,
// so the §28.3 affine auto-drop must-use warning fires (no error — auto-drop is
// a warning, not the discharge error).
BEZEL_TEST(sourceless_static_factory_1499, plain_var_fresh_autodrops_with_warning) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): Res return new Res(); end Make; "
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
BEZEL_TEST(sourceless_static_factory_1499, fresh_using_var_discharges_no_warning) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): Res return new Res(); end Make; "
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
// "the same discharge rules as `new` apply").
BEZEL_TEST(sourceless_static_factory_1499, fresh_owned_return_transfer_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Make(): Res return new Res(); end Make; "
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
        "  public static readonly owned _instance: Config := new Config(); "
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
        "  public static readonly owned _instance: Config := new Config(); "
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
