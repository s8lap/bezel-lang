// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_restoration_path_sensitive_867_test.cpp
//   (BEZEL_TEST field_restoration_path_sensitive_867, loop_partial_restore_rejected).
//
// §28.8.1 / #867: a `move` whose restore is inside a conditionally-entered
// loop body is NOT restored on the zero-trip path (loop never runs) — must be
// REJECTED. The loop back-edge / zero-trip skip meet must carry the moved-from
// fact through.

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public method M(n: int): owned Logfile var d := move _log; var i := 0; while i < n do if i = 5 then _log := new Logfile(); end; i := i + 1; end; return d; end M; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
