// Ported from tests/sema/field_restoration_path_sensitive_867_test.cpp
//   (BEZEL_TEST field_restoration_path_sensitive_867, if_else_both_restore_accepted).
//
// §28.8.1 / #867 (over-reject guard): both if/else branches restore — the
// field is restored on EVERY path, so this must be ACCEPTED. A wrong merge
// (meet instead of OR / dropping the then-branch's restore) would wrongly
// reject this.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public method Swap(c: bool): owned Logfile var d := move _log; if c then _log := new Logfile(); else _log := new Logfile(); end; return d; end Swap; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
