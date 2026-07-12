// Ported from tests/sema/inheritance_cycle_test.cpp
//   (BEZEL_TEST sema_cycle, linear_chain_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A end A; class B : A end B; class C : B end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
