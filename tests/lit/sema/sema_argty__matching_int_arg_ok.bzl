// Ported from tests/sema/call_arg_types_test.cpp
//   (BEZEL_TEST sema_argty, matching_int_arg_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int) end F; public method M() this.F(5); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
