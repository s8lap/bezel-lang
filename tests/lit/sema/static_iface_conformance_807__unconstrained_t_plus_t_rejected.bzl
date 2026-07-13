// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, unconstrained_t_plus_t_rejected).
//
// UNCONSTRAINED `a + b` on T-typed operands stays rejected (§25).

module M; static class F public static method Add2<T>(a: T, b: T): T return a + b; end Add2; end F;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `+` is not defined for type `?`
// CHECK-SAME: (§25)
