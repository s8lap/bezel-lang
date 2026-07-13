// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, arity_mismatch_rejected_spec_27_1_1).
//
// §27.1.1 (arity is a prerequisite for every bullet above): a mismatched
// arity is never assignable.

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var f: (int) -> int := Twice; var g: (int, int) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
