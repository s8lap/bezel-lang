// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, unconstrained_t_plus_t_rejected).
//
// UNCONSTRAINED `a + b` on T-typed operands stays rejected (§25.2/§11.8).
//
// fix #3930 (§25.2/§11.8, v0.295): the diagnostic used to name the operand
// type as the literal placeholder `?` instead of the type parameter `T`
// (`operator `+` is not defined for type `?` (§25); declare `static method
// operator +(a: ?, b: ?)` inside `?` to overload it` — unactionable, a type
// parameter declares no members of its own). Updated to the new, correct
// wording (mirrors the sibling ordering-operator diagnostic).

module M; static class F public static method Add2<T>(a: T, b: T): T return a + b; end Add2; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not available on type parameter `T`
// CHECK-SAME: add a `where T: INumber<T>` bound
// CHECK-SAME: (§25.2/§11.8)
