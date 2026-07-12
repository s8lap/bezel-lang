// Ported from tests/sema/call_arity_test.cpp
//   (BEZEL_TEST sema_arity, zero_arg_call_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F() end F; public method M() this.F(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
