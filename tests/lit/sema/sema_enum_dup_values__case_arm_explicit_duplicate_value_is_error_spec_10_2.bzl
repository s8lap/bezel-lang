// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, case_arm_explicit_duplicate_value_is_error_spec_10_2).
//
// `A := 1, B := 1`; a `case` with an unguarded arm for `A` followed by an
// arm for `B` is a compile error — `B` denotes the same value as `A` and can
// never be reached.

module M; enum E A := 1, B := 1; end E; class C public method M(v: E): int return (case v of A => 1, B => 2); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this `case` arm is unreachable: `B` denotes the same underlying value as `A`, already matched by an earlier unguarded arm
// CHECK-SAME: (§10.2)
