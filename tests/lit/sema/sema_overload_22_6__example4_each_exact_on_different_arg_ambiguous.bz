// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example4_each_exact_on_different_arg_ambiguous).
//
// §22.6 Example 4 — each candidate exact on a DIFFERENT argument: ambiguous.
// Compute(int,long) is exact on arg a, convert on arg b; Compute(long,int)
// is convert on a, exact on b. Neither dominates → compile error.

module M; class Calculator public method Compute(a: int, b: long): int => 1; public method Compute(a: long, b: int): int => 2; public method Use(x: int, y: int) _ := this.Compute(x, y); end Use; end Calculator;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: ambiguous call to `Compute`: no overload is unambiguously better
// CHECK-SAME: (§22.6)
