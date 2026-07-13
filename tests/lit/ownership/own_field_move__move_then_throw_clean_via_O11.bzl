// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/move_on_fields_test.cpp
//   (BEZEL_TEST own_field_move, move_then_throw_clean_via_O11).

module M; class Logfile end Logfile; class C private owned _log: Logfile := new Logfile(); public method Bad() using var old := move this._log; throw new Error(); end Bad; public class Error end Error; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
