// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_types_test.cpp
//   (BEZEL_TEST sema_builtin, idisposable_with_arg_diagnoses).

module M; class C public _x: IDisposable<int>; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: generic-argument count does not match built-in type `IDisposable`
