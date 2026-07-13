// Ported from tests/sema/local_const_type_test.cpp
//   (BEZEL_TEST sema_lconst, int_local_const_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class T end T; class C public method M() const K: int := 5; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
