// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_exceptions_test.cpp
//   (BEZEL_TEST sema_exc, unknown_exception_still_diagnoses).

module M; class C public _e: MadeUpException; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved type name `MadeUpException`
