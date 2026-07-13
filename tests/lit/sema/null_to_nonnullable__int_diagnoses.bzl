// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_to_nonnullable_test.cpp
//   (BEZEL_TEST null_to_nonnullable, int_diagnoses).
//
// REJECT: null into a non-nullable value type.

module M; class Box public value: int := 0; public constructor(v: int) value := v; end constructor; end Box; class C public static method Main(): int var x: int := null; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `x` is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
