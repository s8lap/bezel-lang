// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/const_type_test.cpp
//   (BEZEL_TEST sema_const, object_const_diagnoses).

module M; class T end T; class C public const O: Object := null; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `O` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
