// Ported from tests/ownership/move_on_fields_test.cpp
//   (BEZEL_TEST own_field_move, no_move_clean).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Logfile end Logfile; class C private owned _log: Logfile := new Logfile(); public method Touch() this._log := new Logfile(); end Touch; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
