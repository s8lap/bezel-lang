// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, primitive_param_invariant_rejected_spec_27_1_1).
//
// §27.1.1 third bullet: "value-typed ... primitive slots are INVARIANT. No
// numeric widening ... applies to a slot of an already-constructed function
// value: there is no adapter object to perform it." An (int) -> int VALUE is
// not usable where (long) -> int is expected, even though int widens to long
// as an ordinary value conversion (§5.1).

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var f: (int) -> int := Twice; var g: (long) -> int := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `g` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1)
