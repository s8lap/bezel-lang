// Ported from tests/ownership/assignment_consumes_test.cpp
//   (BEZEL_TEST own_assign_move, field_self_swap_via_move).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C private owned _t: T := new T(); public method Swap() using var old := move this._t; this._t := new T(); end Swap; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
