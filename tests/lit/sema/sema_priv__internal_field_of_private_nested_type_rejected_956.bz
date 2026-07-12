// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, internal_field_of_private_nested_type_rejected_956).
//
// REJECT: internal field whose type is a private nested class.

module M; public class Outer private class NestedPrivate destructor end destructor; end NestedPrivate; internal _x: NestedPrivate?; destructor end destructor; public static method Main(): int => 0; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the field type `NestedPrivate` is private and less visible than `_x` — a type in a member's signature must be at least as visible as the member
// CHECK-SAME: (§13.3)
