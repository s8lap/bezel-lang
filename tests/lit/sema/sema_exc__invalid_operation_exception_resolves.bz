// Ported from tests/sema/builtin_exceptions_test.cpp
//   (BEZEL_TEST sema_exc, invalid_operation_exception_resolves).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _e: InvalidOperationException; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
