// Ported from tests/sema/return_form_test.cpp
//   (BEZEL_TEST sema_return, value_method_returns_value_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(): int return 1; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
