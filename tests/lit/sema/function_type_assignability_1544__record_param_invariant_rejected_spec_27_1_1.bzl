// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, record_param_invariant_rejected_spec_27_1_1).
//
// §27.1.1 third bullet: distinct RECORD types are invariant — R2 is not R1
// even though both are single-int positional records.

module M; record R1(X: int); record R2(X: int); class C public static method TakeR1(r: R1): int return r.X; end TakeR1; public static method Main(): int var f: (R1) -> int := TakeR1; var g: (R2) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
