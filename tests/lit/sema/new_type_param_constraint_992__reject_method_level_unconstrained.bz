// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_type_param_constraint_992_test.cpp
//   (BEZEL_TEST new_type_param_constraint_992, reject_method_level_unconstrained).
//
// §14.1 (#992): `new T()` in a GENERIC METHOD where the method declares no
// constraints at all (unconstrained T) must be rejected.

module M; static class Factory public static method Make<T>(): T return new T(); end Make; end Factory; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new T()` requires the type parameter `T` to be constrained `where T: new()` — the body type-checks under the weakest assumption its constraints establish, and construction is not guaranteed without the `new()` constraint
// CHECK-SAME: (§14.1, #992)
