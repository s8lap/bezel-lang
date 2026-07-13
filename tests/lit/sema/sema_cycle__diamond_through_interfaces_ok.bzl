// Ported from tests/sema/inheritance_cycle_test.cpp
//   (BEZEL_TEST sema_cycle, diamond_through_interfaces_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface I1 method M(); end I1; interface I2 method M(); end I2; class C : I1, I2 public method M() _ := 1; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
