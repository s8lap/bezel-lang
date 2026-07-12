// Ported from tests/sema/accessor_visibility_test.cpp
//   (BEZEL_TEST sema_acc_vis, equal_rank_not_flagged).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C internal property X: int internal set := 0; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
