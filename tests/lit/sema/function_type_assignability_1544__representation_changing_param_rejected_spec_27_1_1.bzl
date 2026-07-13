// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, representation_changing_param_rejected_spec_27_1_1).
//
// §27.1.1 third bullet: a representation-changing conversion between
// unrelated primitive kinds (int vs double — different register class, not
// just a different width) is rejected the same way.

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var f: (int) -> int := Twice; var g: (double) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
