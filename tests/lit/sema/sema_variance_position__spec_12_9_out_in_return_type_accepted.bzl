// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_out_in_return_type_accepted).
//
// §12.9 example C1: `interface IEnumerable<out T> { method GetEnumerator(): ... }`
// T in return type is a valid output position for `out`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IGoodOut<out T> method Get(): T; end IGoodOut;

// NODIAG-NOT: error
// NODIAG-NOT: §
