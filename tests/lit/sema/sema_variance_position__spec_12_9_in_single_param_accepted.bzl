// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_in_single_param_accepted).
//
// §12.9: a single input parameter — valid for `in`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IAction<in T> method Do(item: T); end IAction;

// NODIAG-NOT: error
// NODIAG-NOT: §
