// Ported from tests/sema/field_restoration_path_sensitive_867_test.cpp
//   (BEZEL_TEST field_restoration_path_sensitive_867, restore_before_branch_accepted).
//
// §28.8.1 / #867 (over-reject guard): the restore happens BEFORE the branch,
// so the field is restored on every path regardless of the condition — must
// be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public method Swap(c: bool): owned Logfile var d := move _log; _log := new Logfile(); if c then end; return d; end Swap; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
