// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/local_const_type_test.cpp
//   (BEZEL_TEST sema_lconst, named_type_local_const_diagnoses).

module M; class T end T; class C public method M() const X: T := null; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `X` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
