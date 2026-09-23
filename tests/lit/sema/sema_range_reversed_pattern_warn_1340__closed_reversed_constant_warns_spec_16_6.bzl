// Ported from tests/sema/range_reversed_pattern_warn_1340_test.cpp
//   (BEZEL_TEST sema_range_reversed_pattern_warn_1340, closed_reversed_constant_warns_spec_16_6).
//
// §16.6 v0.162: a closed reversed range `100..0` in a pattern fires the warning.
// The bounds are constant integers with lo=100 > hi=0, so the pattern can
// never match (equivalent to `50 in 100..0` returning false for any scrutinee).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method M(x: int): int return (case x of 100..0 => 1, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} this pattern never matches — bounds are reversed
// CHECK-SAME: (§16.6)
