// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, if_int_condition_diagnoses).

module M; class C public method M(flag: bool, n: int) if n then n := 1; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the if condition must be `bool`
// CHECK-SAME: (§21.2)
