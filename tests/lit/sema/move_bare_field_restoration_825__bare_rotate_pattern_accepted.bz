// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, bare_rotate_pattern_accepted).
//
// §28.8.1: both bare and qualified forms accept the Rotate pattern
// (move-and-restore with an intervening use of the moved-out value).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile public constructor() end constructor; destructor end destructor; end Logfile; class Holder private owned _log: Logfile; public constructor() _log := new Logfile(); end constructor; public class Logfile2 public constructor() end constructor; destructor end destructor; end Logfile2; public method Rotate(): owned Logfile var old := move _log; _log := new Logfile(); return old; end Rotate; destructor end destructor; end Holder; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
