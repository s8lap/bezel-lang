// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_method_constraint_callsite_746_test.cpp
//   (BEZEL_TEST method_constraint_callsite_746, unconstrained_method_param_t_method_rejected_1078).
//
// §14.2 (#1078): a method call on an unconstrained generic METHOD parameter T
// is rejected — the method's `x: T` (no where-clause) provides no `GetN()`.

module M; static class U public static method Wrap<T>(x: T): int x.GetN(); return 0; end Wrap; end U; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `GetN` on unconstrained type parameter `T`; add a `where T: <constraint>` that provides this member
// CHECK-SAME: (§14.2)
