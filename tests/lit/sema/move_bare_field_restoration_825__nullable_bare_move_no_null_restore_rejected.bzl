// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, nullable_bare_move_no_null_restore_rejected).
//
// §28.8.1: nullable owned field, bare `move _log` with no null restoration
// must be rejected.

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile?; public constructor() _log := new Logfile(); end constructor; public method Detach(): owned Logfile? return move _log; end Detach; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
