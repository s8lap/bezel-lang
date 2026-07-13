// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/move_on_fields_test.cpp
//   (BEZEL_TEST own_field_move, move_without_restore_diagnosed).

module M; class Logfile end Logfile; class C private owned _log: Logfile := new Logfile(); public method Detach(): Logfile return move this._log; end Detach; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot return an owned field's `move`d value through a non-`owned` return
// CHECK-SAME: (§28.6)
