// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, primitive_return_invariant_rejected_spec_27_1_1).
//
// §27.1.1 third bullet, RETURN slot: a () -> int value is not usable as
// () -> long — same invariance, applied to the return slot.

module M; class C public static method GetInt(): int return 5; end GetInt; public static method Main(): int var f: () -> int := GetInt; var g: () -> long := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
