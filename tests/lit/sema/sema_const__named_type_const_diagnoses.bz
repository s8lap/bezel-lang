// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, named_type_const_diagnoses).

module M; class T end T; class C public const X: T := null; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `X` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
