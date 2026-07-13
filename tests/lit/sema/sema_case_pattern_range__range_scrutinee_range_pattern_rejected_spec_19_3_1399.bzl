// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, range_scrutinee_range_pattern_rejected_spec_19_3_1399).
//
// §19.3 (#1399): the issue's exact repro — `case (lo..hi) of 1..3 => ...`.
// The scrutinee `(lo..hi)` has type Range; the pattern `1..3` is an integer
// range pattern. Was a compiler SIGABRT; must now produce a clean sema error.

module M; class C public static method Main(): int var lo := 1; var hi := 3; var r := case (lo..hi) of 1..3 => "x", _ => "other", end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: range or relational pattern is not compatible with a Range-typed scrutinee — a Range value is a struct, not a scalar; use `_` or `var x` to match
// CHECK-SAME: (§19.3)
