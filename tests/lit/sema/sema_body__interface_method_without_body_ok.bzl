// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, interface_method_without_body_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface I method F(): int; end I;

// NODIAG-NOT: error
// NODIAG-NOT: §
