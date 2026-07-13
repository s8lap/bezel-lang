// Ported from tests/sema/static_ctor_params_test.cpp
//   (BEZEL_TEST sema_static_ctor, no_params_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C static constructor() end constructor; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
