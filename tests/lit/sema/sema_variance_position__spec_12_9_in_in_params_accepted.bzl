// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_in_in_params_accepted).
//
// §12.9 example D1: `interface IComparer<in T> { method Compare(a: T, b: T): int; }`
// T in method parameters — input positions — is valid for `in`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IGoodIn<in T> method Compare(a: T, b: T): int; end IGoodIn;

// NODIAG-NOT: error
// NODIAG-NOT: §
