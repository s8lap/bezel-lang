// Spec-derived tests for Ownership slice O9 — owned-field definite
// assignment in constructors (§28.6).
//
// An `owned` field without an in-class initializer is the constructor's
// responsibility: every constructor must assign the field on every
// normal-exit path. A `this(...)` chained constructor transfers the
// responsibility to the chained target; a `base(...)` chain does not.
//
// Slice O9 is a syntactic over-approximation: each non-this-chained
// constructor body is required to contain at least one assignment
// `this.<field> := <expr>` for every owned-without-initializer field.
// A constructor that initializes the field only on some control-flow
// paths is not flagged here yet — full DA awaits the CFG-based pass.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; class T end T; ") + body;
}

} // namespace

BEZEL_TEST(own_field_da, owned_field_without_init_assigned_in_ctor_ok) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    this._t := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, owned_field_without_init_missing_assignment_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, owned_field_with_initializer_ok_without_assignment) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T := new T();"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, non_owned_field_not_required) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private _t: T;"
        "  public constructor() "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, multiple_ctors_each_must_assign) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T;"
        "  public constructor() "
        "    this._t := new T(); "
        "  end constructor; "
        "  public constructor(seed: int) "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, this_chained_ctor_transfers_responsibility) {
    // The seed ctor delegates to the no-arg ctor via `this(...)` —
    // it no longer has to assign the owned field itself.
    auto r = parseAndCheckOwnership(mod(
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
    auto r = parseAndCheckOwnership(mod(
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
}

BEZEL_TEST(own_field_da, no_constructor_with_owned_field_diagnoses) {
    // No constructor declared but owned-without-init field exists.
    // The synthesized default ctor cannot satisfy the field — diagnose
    // at the field declaration.
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _t: T;"
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_da, multiple_owned_fields_all_required) {
    auto r = parseAndCheckOwnership(mod(
        "class C "
        "  private owned _a: T;"
        "  private owned _b: T;"
        "  public constructor() "
        "    this._a := new T(); "
        "  end constructor; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
