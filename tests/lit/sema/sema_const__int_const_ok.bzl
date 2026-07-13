// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, int_const_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public const K: int := 5; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
