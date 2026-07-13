// Ported from tests/sema/base_visibility_test.cpp
//   (BEZEL_TEST sema_base_vis, internal_base_internal_derived_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; internal class B end B; internal class D : B end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
