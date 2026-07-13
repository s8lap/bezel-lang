// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, elif_int_condition_diagnoses).

module M; class C public method M(flag: bool, n: int) if flag then n := 1; elif n then n := 2; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the elif condition must be `bool`
// CHECK-SAME: (§21.2)
