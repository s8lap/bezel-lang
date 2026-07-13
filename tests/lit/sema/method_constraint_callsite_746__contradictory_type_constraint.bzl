// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, contradictory_type_constraint).
//
// §14.2: a generic TYPE with `where T: class, value` is rejected (even though
// it is never instantiated).

module M; class Foo<T> where T: class, value end Foo; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: contradictory constraints on type parameter `T`: `class` and `value` cannot both be required
// CHECK-SAME: (§14.2)
