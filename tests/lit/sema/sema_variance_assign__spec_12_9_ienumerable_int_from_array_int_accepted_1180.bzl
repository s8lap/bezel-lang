// Ported from tests/sema/generic_variance_assign_1180_test.cpp
//   (BEZEL_TEST sema_variance_assign, spec_12_9_ienumerable_int_from_array_int_accepted_1180).
//
// Exact value-type match: Array<int> → IEnumerable<int> — exact arg, always OK.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(a: Array<int>) var x: IEnumerable<int> := a; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
