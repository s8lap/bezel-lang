// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, qualified_no_restore_rejected).
//
// §28.8.1 (regression guard): qualified `move this._log` with no restoration
// must remain rejected.  This was already rejected before #825 and must not
// regress.

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public method Detach(): owned Logfile return move this._log; end Detach; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
