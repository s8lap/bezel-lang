// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, if_nullable_bool_condition_diagnoses_r39).

module M; class C public method M(flag: bool, n: int) var b: bool? := null; if b then n := 1; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the if condition must be `bool`, not `bool?` — a nullable bool cannot be used in a boolean position
// CHECK-SAME: (§21.2, §7.11)
