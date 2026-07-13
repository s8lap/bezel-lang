// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, dispose_with_params_ok).
//
// A Dispose method with parameters is NOT the synthesised Dispose() signature
// (§28.2 only restricts the zero-parameter form) — this is an unusual but
// technically distinct method that happens to share the name.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method Dispose(reason: string) _ := 0; end Dispose; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
