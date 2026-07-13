// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, record_in_record_no_cycle_not_rejected_1272).
//
// CONTROL: a record containing a DIFFERENT (non-cyclic) record is fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Inner(x: int); record Outer(i: Inner, y: int);

// NODIAG-NOT: error
// NODIAG-NOT: §
