// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, internal_method_returning_private_type_rejected_956).
//
// REJECT: internal method whose return type is a private nested class.

module M; public class Outer private class Token destructor end destructor; end Token; internal method MakeToken(): owned Token => new Token(); destructor end destructor; public static method Main(): int => 0; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the return type `Token` is private and less visible than `MakeToken` — a type in a member's signature must be at least as visible as the member
// CHECK-SAME: (§13.3)
