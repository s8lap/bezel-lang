// Ported from tests/sema/accessor_visibility_test.cpp
//   (BEZEL_TEST sema_acc_vis, private_set_on_public_prop_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public property X: int private set := 0; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
