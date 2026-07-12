// Ported from tests/sema/multi_base_test.cpp
//   (BEZEL_TEST sema_multi_base, multiple_interfaces_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface I1 method M1(); end I1; interface I2 method M2(); end I2; class C : I1, I2 public method M1() _ := 1; end M1; public method M2() _ := 2; end M2; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
