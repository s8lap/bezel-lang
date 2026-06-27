// fix #825 / §28.8.1: enforce the field-restoration rule for unqualified
// `move _field`.  Before the fix, a bare `move _log` bypassed the §28.8.1
// check that the qualified `move this._log` form already enforces.  Both
// syntactic forms must behave identically: the moved-from field must be
// reassigned on every path before method exit.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Wrapper: a class with an owned Logfile field, a no-op constructor, and a
// no-op destructor.  Individual tests inline the method under scrutiny.
std::string withField(const std::string& method) {
    return std::string(
        "module M; "
        "class Logfile "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Logfile; "
        "class Holder "
        "  private owned _log: Logfile; "
        "  public constructor() _log := new Logfile(); end constructor; ") +
        method +
        " destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;";
}

std::string withNullableField(const std::string& method) {
    return std::string(
        "module M; "
        "class Logfile "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Logfile; "
        "class Holder "
        "  private owned _log: Logfile?; "
        "  public constructor() _log := new Logfile(); end constructor; ") +
        method +
        " destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;";
}

// True iff some emitted diagnostic mentions the §28.8.1 field-restoration rule.
bool hasFieldRestorationError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("left in moved-from state") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §28.8.1 / #825: bare `move _log` with no restoration must be rejected.
// Before the fix this was silently accepted.
BEZEL_TEST(move_bare_field_restoration_825, bare_no_restore_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method Detach(): owned Logfile "
        "  return move _log; "
        "end Detach;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 (regression guard): qualified `move this._log` with no restoration
// must remain rejected.  This was already rejected before #825 and must not
// regress.
BEZEL_TEST(move_bare_field_restoration_825, qualified_no_restore_rejected) {
    auto r = parseAndCheckOwnership(withField(
        "public method Detach(): owned Logfile "
        "  return move this._log; "
        "end Detach;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1: bare `move _log` followed by a bare `_log := new Logfile()` restore
// must be accepted.
BEZEL_TEST(move_bare_field_restoration_825, bare_with_restore_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "public method Replace(): owned Logfile "
        "  var d := move _log; "
        "  _log := new Logfile(); "
        "  return d; "
        "end Replace;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: nullable owned field — bare `move _log; _log := null; return d;`
// is the spec-correct detach pattern (null restores a nullable owned field).
// Must be accepted.
BEZEL_TEST(move_bare_field_restoration_825, nullable_bare_move_null_restore_accepted) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Detach(): owned Logfile? "
        "  var d := move _log; "
        "  _log := null; "
        "  return d; "
        "end Detach;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: nullable owned field, bare `move _log` with no null restoration
// must be rejected.
BEZEL_TEST(move_bare_field_restoration_825, nullable_bare_move_no_null_restore_rejected) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Detach(): owned Logfile? "
        "  return move _log; "
        "end Detach;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 (#1125): `move _log!` — a force-unwrap (null-forgiving `!`) of a
// nullable owned field — is a move of `_log` exactly like `move _log`, so the
// field is left moved-from and must be restored before exit. Before the fix the
// `!` form (a PostfixBangExpr wrapping the field) bypassed the field-move
// recognition entirely, so this compiled with NO diagnostic — and the owner's
// auto-disposer then double-freed the moved-out object (deterministic SIGSEGV).
// It must be rejected with the same §28.8.1 diagnostic as the bare form.
BEZEL_TEST(move_bare_field_restoration_825, nullable_force_unwrap_move_no_restore_rejected) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Take(): owned Logfile "
        "  return move _log!; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 (#1125): the qualified force-unwrap form `move this._log!` must be
// rejected identically.
BEZEL_TEST(move_bare_field_restoration_825, qualified_force_unwrap_move_no_restore_rejected) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Take(): owned Logfile "
        "  return move this._log!; "
        "end Take;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFieldRestorationError(r));
}

// §28.8.1 (#1125 / spec line 3529): the canonical nullable-owned-field detach
// `move _log!` immediately followed by `_log := null` IS the spec-endorsed
// pattern and must compile cleanly (the field is restored before exit).
BEZEL_TEST(move_bare_field_restoration_825, nullable_force_unwrap_move_null_restore_accepted) {
    auto r = parseAndCheckOwnership(withNullableField(
        "public method Take(): owned Logfile "
        "  var d := move _log!; "
        "  _log := null; "
        "  return d; "
        "end Take;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: a bare `move` of a LOCAL variable (not a field) must not trigger
// the field-restoration check — exprIsOwnedFieldRead correctly returns false
// for a non-field local.
BEZEL_TEST(move_bare_field_restoration_825, bare_local_move_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var r := new R(); "
        "  var s := move r; "
        "  s.Dispose(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.8.1: both bare and qualified forms accept the Rotate pattern
// (move-and-restore with an intervening use of the moved-out value).
BEZEL_TEST(move_bare_field_restoration_825, bare_rotate_pattern_accepted) {
    auto r = parseAndCheckOwnership(withField(
        "class Logfile2 "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Logfile2; "
        // Note: Logfile2 is a different class so Rotate has a valid
        // non-owned method to call; since we cannot call methods with
        // no context, we just verify the move+restore compiles cleanly.
        "public method Rotate(): owned Logfile "
        "  var old := move _log; "
        "  _log := new Logfile(); "
        "  return old; "
        "end Rotate;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
