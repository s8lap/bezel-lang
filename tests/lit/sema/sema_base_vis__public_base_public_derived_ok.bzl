// Ported from tests/sema/base_visibility_test.cpp
//   (BEZEL_TEST sema_base_vis, public_base_public_derived_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class B end B; public class D : B end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
