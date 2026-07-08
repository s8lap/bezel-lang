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

// A static method whose body returns a static-field READ (`_shared`) is a
// BORROW. Its call-site result must NOT be flagged Owned — `var r := Get();`
// carries no auto-drop obligation. Classifying it owned is the exact soundness
// regression the previous attempt introduced (it would auto-drop / dispose the
// process-lived shared object — a double-free, §28.10). The §22.12-rule-4
// domain warning on the static readonly field is unrelated and is a Warning,
// not an error, so hasErrors() stays false.
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
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "r"));
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
