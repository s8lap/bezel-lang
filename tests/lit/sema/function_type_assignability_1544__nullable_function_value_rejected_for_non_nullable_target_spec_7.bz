// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, nullable_function_value_rejected_for_non_nullable_target_spec_7).
//
// §7 (general nullable rule, unaffected by the Function-Function block
// existing): a nullable function-typed value is not usable where a
// non-nullable function type is expected.

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var f: ((int) -> int)? := Twice; var g: (int) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
