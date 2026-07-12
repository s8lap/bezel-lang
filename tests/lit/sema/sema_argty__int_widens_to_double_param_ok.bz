// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, int_widens_to_double_param_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: double) end F; public method M() this.F(5); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
