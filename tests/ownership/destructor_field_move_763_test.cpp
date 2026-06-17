// Spec-derived tests for moving an owned field inside a destructor (issue #763).
//
// §28.8.1 (spec line ~5305), "Constructors and destructors": "Inside a
// destructor body, the field is being read for final disposal. `move x.f`
// inside a destructor takes the field's value out, which causes the compiler-
// generated auto-dispose pass for that field to skip it … The taken value must
// still be discharged … the field-restoration rule does *not* require
// reassignment inside a destructor, because the object is being torn down and no
// future read of the field will occur."
//
// The under-enforcement this file pins down (and #763 fixes): the field-moved-
// from-at-method-exit check fired unconditionally, including inside destructor
// bodies, so `var taken := move this._inner; taken.Dispose();` in a destructor
// was spuriously rejected ("field `_inner` left in moved-from state at method
// exit"). The check is now skipped for destructor bodies; it still fires for
// every other method/constructor body.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool hasFieldMovedFromError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("left in moved-from state at method exit") !=
            std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// =============================================================================
// ACCEPTED — moving an owned field in a destructor (§28.8.1).
// =============================================================================

// §28.8.1: `var taken := move this._inner; taken.Dispose();` in a destructor —
// the field need not be restored (the object is being torn down) and the taken
// value is discharged. ACCEPTED (the #763 repro).
BEZEL_TEST(destructor_field_move_763, destructor_move_then_dispose_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Outer "
        "  private owned _inner: Inner; "
        "  public constructor() this._inner := new Inner(); end constructor; "
        "  destructor "
        "    var taken := move this._inner; "
        "    taken.Dispose(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(hasFieldMovedFromError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: the canonical `using var u := move this._inner;` "take the field's
// value out and consume it scope-bound" form in a destructor. ACCEPTED.
BEZEL_TEST(destructor_field_move_763, destructor_move_into_using_var_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Outer "
        "  private owned _inner: Inner; "
        "  public constructor() this._inner := new Inner(); end constructor; "
        "  destructor "
        "    using var u := move this._inner; "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(hasFieldMovedFromError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// STILL REJECTED — the field-restoration rule applies outside destructors.
// =============================================================================

// §28.8.1: a regular METHOD that moves an owned field out and does not restore
// it leaves the field's invariant broken on exit — STILL REJECTED (the
// destructor exemption must not leak into ordinary methods).
BEZEL_TEST(destructor_field_move_763, regular_method_field_not_restored_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Outer "
        "  private owned _inner: Inner; "
        "  public constructor() this._inner := new Inner(); end constructor; "
        "  public method Take(): () "
        "    using var u := move this._inner; "
        "  end Take; "
        "  destructor this._inner.Dispose(); end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(hasFieldMovedFromError(r));
}

// §28.3: the destructor exemption is scoped to the FIELD-restoration rule only;
// the ordinary owned-LOCAL discharge rule still applies inside a destructor — a
// plain `new` owned local that is never discharged is STILL REJECTED.
BEZEL_TEST(destructor_field_move_763, destructor_undischarged_owned_local_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Outer "
        "  destructor "
        "    var x := new Inner(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
