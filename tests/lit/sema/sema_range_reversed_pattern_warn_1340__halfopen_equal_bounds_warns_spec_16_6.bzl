// Ported from tests/sema/range_reversed_pattern_warn_1340_test.cpp
//   (BEZEL_TEST sema_range_reversed_pattern_warn_1340, halfopen_equal_bounds_warns_spec_16_6).
//
// §16.6 v0.162: a half-open equal-bound `5..<5` fires the warning.
// lo >= hi for half-open: the interval [5,5) is empty.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method M(x: int): int return (case x of 5..<5 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} this pattern never matches — bounds are reversed
// CHECK-SAME: (§16.6)
