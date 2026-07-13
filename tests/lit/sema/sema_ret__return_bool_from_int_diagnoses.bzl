// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/return_assignability_test.cpp
//   (BEZEL_TEST sema_ret, return_bool_from_int_diagnoses).

module M; class C public method M(): int return true; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the returned value has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
