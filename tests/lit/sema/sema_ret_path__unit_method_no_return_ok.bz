// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, unit_method_no_return_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F() var x := 1; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
