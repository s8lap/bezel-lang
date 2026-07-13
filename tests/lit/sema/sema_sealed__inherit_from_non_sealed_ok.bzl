// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, inherit_from_non_sealed_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base end Base; class Derived : Base end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
