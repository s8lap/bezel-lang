// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, case_arm_auto_increment_collision_is_error_spec_10_2).
//
// `A := 1, B, C := 2`; `B` auto-increments to 2 and collides with the
// explicit `C := 2`. A `case` naming `B` then `C` is a compile error exactly
// as the explicit-duplicate case above (§10.2: "whether written explicitly
// ... or produced by auto-increment continuation").

module M; enum E A := 1, B, C := 2; end E; class D public method M(v: E): int return (case v of B => 1, C => 2, _ => 0); end M; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this `case` arm is unreachable: `C` denotes the same underlying value as `B`, already matched by an earlier unguarded arm
// CHECK-SAME: (§10.2)
