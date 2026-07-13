// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/type_param_bound_1620_test.cpp
//   (BEZEL_TEST type_param_bound_1620, member_surface_unconstrained_u_still_rejected).
//
// §14.2 ANTI-OVER-REJECT: with `U` unconstrained, `T` gets only Object's
// surface — a non-Object member on `T` stays rejected (the chain adds nothing).

module M; class Animal public method Speak(): string return "..."; end Speak; end Animal; static class Utils public static method Describe<T, U>(x: T): string where T: U return x.Speak(); end Describe; end Utils; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Speak` on unconstrained type parameter `T`; add a `where T: <constraint>` that provides this member
// CHECK-SAME: (§14.2)
