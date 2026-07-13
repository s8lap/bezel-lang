// Ported from tests/sema/overload_signature_test.cpp
//   (BEZEL_TEST sema_overload, different_named_methods_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int) _ := 1; end F; public method G(a: int) _ := 2; end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
