// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_invariant_both_positions_accepted).
//
// An invariant interface with T in both positions — always valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IList<T> method Add(item: T); method Get(): T; end IList;

// NODIAG-NOT: error
// NODIAG-NOT: §
