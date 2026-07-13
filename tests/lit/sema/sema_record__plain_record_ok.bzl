// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, plain_record_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public _x: int := 0; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §
