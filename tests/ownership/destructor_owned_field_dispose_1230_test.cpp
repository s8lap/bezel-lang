// Regression tests for issue #1230: explicit .Dispose() of an owned field
// inside a destructor body double-frees.
//
// §28.4: the compiler-generated Dispose() runs the destructor body THEN
// auto-disposes all owned-bearing fields. An explicit `_field.Dispose()` (or
// `this._field.Dispose()`) in the destructor disposes the field TWICE —
// double-free / heap-UAF. The fix is a §28.5 sema error rejecting any explicit
// `.Dispose()` call whose receiver is an owned field of the current object
// (`_f` bare-identifier or `this._f`) inside a destructor body.
//
// Anti-over-reject surface:
//   - `.Dispose()` on a LOCAL / using-var inside a destructor: ACCEPTED
//   - `.Dispose()` on a `move`d-out owned field inside a destructor: ACCEPTED
//   - `.Dispose()` on an owned field in a NORMAL method: ACCEPTED
//   - `.Dispose()` on a non-owned field: N/A (non-owned fields have no Dispose)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool hasDoubleDisposeError(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("explicit `.Dispose()` of an owned field inside a "
                           "destructor double-frees") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// REJECTED — explicit .Dispose() on an owned THIS field in a destructor.
// §28.4 / §28.5
// =============================================================================

// §28.4 / §28.5 (#1230 repro): bare `_sub.Dispose()` in a destructor where
// `_sub` is an owned field — the auto-dispose also runs → double-free. REJECTED.
BEZEL_TEST(destructor_owned_field_dispose_1230,
           bare_field_dispose_in_destructor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Widget "
        "  private owned _sub: Inner; "
        "  public constructor() _sub := new Inner(); end constructor; "
        "  destructor "
        "    _sub.Dispose(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_TRUE(hasDoubleDisposeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §28.4 / §28.5 (#1230): explicit-`this` form `this._sub.Dispose()` in a
// destructor — same double-free hazard, same diagnostic. REJECTED.
BEZEL_TEST(destructor_owned_field_dispose_1230,
           explicit_this_field_dispose_in_destructor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Widget "
        "  private owned _sub: Inner; "
        "  public constructor() this._sub := new Inner(); end constructor; "
        "  destructor "
        "    this._sub.Dispose(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_TRUE(hasDoubleDisposeError(r));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// =============================================================================
// ACCEPTED — anti-over-reject surface.
// =============================================================================

// §28.5 (#1230 anti-over-reject): `.Dispose()` on a using-var LOCAL inside a
// destructor is NOT an owned-field call — the local is a new owned resource
// created in the destructor body with no auto-cleanup counterpart. ACCEPTED.
BEZEL_TEST(destructor_owned_field_dispose_1230,
           using_var_dispose_in_destructor_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Outer "
        "  destructor "
        "    using var x := new Inner(); "
        "    x.Dispose(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Outer;");
    BEZEL_EXPECT_FALSE(hasDoubleDisposeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1 / §28.5 (#1230 anti-over-reject): `move _field` in a destructor
// takes ownership out and suppresses the auto-cleanup for that field — using
// a local `var taken := move _sub; taken.Dispose();` is the canonical
// early-dispose pattern and must NOT be rejected. ACCEPTED.
BEZEL_TEST(destructor_owned_field_dispose_1230,
           move_then_dispose_in_destructor_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Widget "
        "  private owned _sub: Inner; "
        "  public constructor() _sub := new Inner(); end constructor; "
        "  destructor "
        "    var taken := move _sub; "
        "    taken.Dispose(); "
        "  end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_FALSE(hasDoubleDisposeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.5 (#1230 anti-over-reject): explicit `.Dispose()` of an owned field in a
// NORMAL (non-destructor) method is a legal explicit-dispose pattern (the field
// must be reassigned or moved-from before the method exits). The destructor-body
// restriction does NOT apply outside the destructor. ACCEPTED (no double-free
// because the destructor auto-cleanup only runs once).
// Note: the field IS restored here (_sub := new Inner()) to satisfy §28.8.1.
BEZEL_TEST(destructor_owned_field_dispose_1230,
           owned_field_dispose_in_normal_method_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Inner public n: int := 0; destructor end destructor; end Inner; "
        "class Widget "
        "  private owned _sub: Inner; "
        "  public constructor() _sub := new Inner(); end constructor; "
        "  public method Reset(): () "
        "    using var old := move _sub; "
        "    _sub := new Inner(); "
        "  end Reset; "
        "  destructor end destructor; "
        "  public static method Main(): int return 0; end Main; "
        "end Widget;");
    BEZEL_EXPECT_FALSE(hasDoubleDisposeError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
