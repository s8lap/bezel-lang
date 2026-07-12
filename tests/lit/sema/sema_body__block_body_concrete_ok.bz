// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, block_body_concrete_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F() return; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
