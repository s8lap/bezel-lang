// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_out_in_readonly_property_accepted).
//
// §12.9: T in a readonly property — pure output — is valid for `out`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IGoodProp<out T> readonly property Current: T; end IGoodProp;

// NODIAG-NOT: error
// NODIAG-NOT: §
