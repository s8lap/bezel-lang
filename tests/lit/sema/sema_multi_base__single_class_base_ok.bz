// Ported from tests/sema/multi_base_test.cpp
//   (BEZEL_TEST sema_multi_base, single_class_base_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base end Base; class Derived : Base end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
