// Ported from tests/sema/record_base_restrictions_test.cpp
//   (BEZEL_TEST sema_record_base_restrictions, plain_record_no_base_ok).
//
// (f) §9.6 CONTROL: plain record with no base list compiles cleanly.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R(X: int) end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
