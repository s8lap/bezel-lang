// Ported from tests/sema/multi_base_test.cpp
//   (BEZEL_TEST sema_multi_base, class_plus_interface_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base end Base; interface I method M(); end I; class C : Base, I public method M() _ := 1; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
