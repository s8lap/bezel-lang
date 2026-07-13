// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, constructor_bare_return_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public constructor() return; end constructor; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
