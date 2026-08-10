// Spec-derived tests for §7.9 (Appendix C.1) — owned/non-nullable-reference
// field definite assignment in constructors.
//
// A non-nullable reference field without an in-class initializer is the
// constructor's responsibility: every constructor must assign the field on
// every normal-exit path. A `this(...)` chained constructor transfers the
// responsibility to the chained target; a `base(...)` chain does not.
//
// Enforcement spans two passes (see checkCtorFieldCompletion in
// DefiniteAssignment.cpp and checkOwnedFieldDA in Ownership.cpp): the DA pass
// checks per-constructor normal-exit completeness with the correct ALL-paths
// field-assignment join (so a field assigned on only some control-flow paths
// IS now flagged — #1823 — and one assigned on every live path where a sibling
// arm diverges is accepted — #1755), and the ownership pass reports the
// no-constructor case. These tests drive BOTH via parseAndCheckFieldInit, the
// combination the real `bezelc` runs. Path-sensitive accept/reject cases (if
// without else, while/for-only, diverging case arm, early return) live in the
// lit track (tests/lit/sema/owned_field_da_1823*.bzl); this file covers the
// declaration-shape matrix.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckFieldInit;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; class T end T; ") + body;
}

} // namespace

BEZEL_TEST(own_field_da, owned_field_without_init_assigned_in_ctor_ok) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    this._t := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, owned_field_without_init_missing_assignment_diagnoses) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_t` (§7.9"));
}

BEZEL_TEST(own_field_da, owned_field_with_initializer_ok_without_assignment) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T := new T();"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, value_type_field_not_required) {
    // §7.9: value-type fields (here `int`) have a well-defined default,
    // so they need no initializer or constructor assignment.
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private _n: int;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, non_owned_reference_field_required) {
    // §7.9: a non-nullable reference field (class type) without an
    // initializer must be assigned in the constructor — even when not
    // `owned`.
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private _t: T;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_t` (§7.9"));
}

BEZEL_TEST(own_field_da, nullable_reference_field_not_required) {
    // A nullable reference field defaults to null — no obligation.
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private _t: T?;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, multiple_ctors_each_must_assign) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    this._t := new T(); "
        "  end constructor; "
        "  public constructor(seed: int) "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_t` (§7.9"));
}

BEZEL_TEST(own_field_da, this_chained_ctor_transfers_responsibility) {
    // The seed ctor delegates to the no-arg ctor via `this(...)` —
    // it no longer has to assign the owned field itself.
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    this._t := new T(); "
        "  end constructor; "
        "  public constructor(seed: int) "
        "    this(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, base_chained_ctor_does_not_transfer) {
    // `base(...)` only initializes the base portion. The derived class's
    // owned field is still this ctor's responsibility — diagnose absence.
    auto r = parseAndCheckFieldInit(mod(
        "class B "
        "  public constructor() end constructor; "
        "end B; "
        "class C : B "
        "  private owned _t: T;"
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_t` (§7.9"));
}

BEZEL_TEST(own_field_da, no_constructor_with_owned_field_diagnoses) {
    // No constructor declared but owned-without-init field exists.
    // The synthesized default ctor cannot satisfy the field — diagnose
    // at the field declaration.
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_t` needs an initializer"));
}

BEZEL_TEST(own_field_da, multiple_owned_fields_all_required) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _a: T;"
        "  private owned _b: T;"
        "  public constructor() "
        "    this._a := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "non-nullable field `_b` (§7.9"));
}

// §7.9 requires the field be "assigned" — it does not mandate `this.`
// qualification. A bare (implicit-`this`) field assignment in the
// constructor satisfies definite assignment, just as `this.<field>` does.
BEZEL_TEST(own_field_da, owned_field_assigned_bare_implicit_this_ok) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    _t := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The same for a plain (non-owned) non-nullable reference field.
BEZEL_TEST(own_field_da, nonnull_field_assigned_bare_implicit_this_ok) {
    auto r = parseAndCheckFieldInit(mod(
        "class C "
        "  public owned _t: T;"  // owned: a fresh `new T()` into a non-`owned`
                                 // field would leak (#1020 / §28.6 v0.153); the
                                 // DA intent (bare-implicit-this init) is unchanged.
        "  public constructor() "
        "    _t := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
