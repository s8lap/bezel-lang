// Spec-derived tests for Ownership slice O8 — `move this.field`
// semantics (§28.8.1; Appendix C.3).
//
// `move this.f` leaves `this.f` in moved-from state. The field must
// be reassigned before the surrounding method exits — otherwise the
// field's invariant is broken at method return.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string wrap(const std::string& method) {
    return std::string("module M; "
        "class Logfile end Logfile; "
        "class C "
        "  private owned _log: Logfile := new Logfile(); "
        ) + method + " end C;";
}

} // namespace

BEZEL_TEST(own_field_move, move_without_restore_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Detach(): Logfile "
        "  return move this._log; "
        "end Detach;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_move, move_then_restore_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Rotate() "
        "  using var old := move this._log; "
        "  this._log := new Logfile(); "
        "end Rotate;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_move, no_move_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Touch() "
        "  this._log := new Logfile(); "
        "end Touch;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_field_move, move_then_throw_clean_via_O11) {
    // Body's last stmt is throw → O11 simple suppresses the local-undischarged
    // check. The field-move check is separate, though — `_log` is still in
    // moved-from state on the exception path. §28.3's exception-path discharge
    // covers the implicit-dispose of the moved value, but the field is left
    // logically empty, which is moot if the object itself is being destroyed
    // (exception unwinding). Slice O8 currently still flags this; future O11
    // CFG-based version distinguishes.
    auto r = parseAndCheckOwnership(wrap(
        "public method Bad() "
        "  using var old := move this._log; "
        "  throw new Error(); "
        "end Bad; "
        "public class Error end Error;"));
    // Currently expected to diagnose because the field-move check runs
    // unconditionally. Mark as known-pessimistic-diagnostic.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
