// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, range_scrutinee_range_pattern_stmt_rejected_spec_19_3_1399).
//
// §19.3 (#1399): statement-position `case` over a Range scrutinee with a
// range pattern is also rejected — CaseStmt and CaseExpr share the same sema
// gate (checkCasePatternConstantRanges).

module M; class C public static method Main() var r := 1..10; case r of 1..5 => do return; end, _ => do return; end end; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: range or relational pattern is not compatible with a Range-typed scrutinee — a Range value is a struct, not a scalar; use `_` or `var x` to match
// CHECK-SAME: (§19.3)
