// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_restoration_path_sensitive_867_test.cpp
//   (BEZEL_TEST field_restoration_path_sensitive_867, conditional_partial_restore_bare_rejected).
//
// §28.8.1 / #867: bare `move _log` restored only on the `c` path is left
// moved-from on the `c == false` path at method exit — must be REJECTED.
// This is the headline bug: before the fix it was silently accepted because
// the moved-from fact was not merged at the if-join.

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public method Maybe(c: bool): owned Logfile var d := move _log; if c then _log := new Logfile(); end; return d; end Maybe; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: field `_log` left in moved-from state at method exit
// CHECK-SAME: (§28.8.1 — reassign the field before exiting)
