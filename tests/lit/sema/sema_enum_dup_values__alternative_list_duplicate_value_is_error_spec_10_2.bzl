// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, alternative_list_duplicate_value_is_error_spec_10_2).

module M; enum E A := 1, B := 1, C := 2; end E; class D public method M(v: E): int return (case v of A | B => 1, C => 2); end M; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `B` denotes the same underlying value as `A` in this `|` alternative list — duplicate enum constant values are the same value, not distinct alternatives
// CHECK-SAME: (§10.2)
