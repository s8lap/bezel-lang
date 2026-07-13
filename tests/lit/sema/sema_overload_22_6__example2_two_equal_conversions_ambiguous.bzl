// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example2_two_equal_conversions_ambiguous).
//
// §22.6 Example 2 — two equally-ranked conversions are ambiguous.
// int -> long and int -> float are both §5.1 implicit and co-equal; the
// call to Convert(a) with `a: int` is a compile error.

module M; class Converter public method Convert(n: long): int => 1; public method Convert(n: float): int => 2; public method Use(a: int) _ := this.Convert(a); end Use; end Converter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: ambiguous call to `Convert`: no overload is unambiguously better
// CHECK-SAME: (§22.6)
