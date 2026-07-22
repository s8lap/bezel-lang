// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, case_new_record_pattern_rejected_1205).
//
// §A.7/§19.3: `new Record(…)` in a case arm pattern is rejected — a
// constructor call is not a valid pattern constant. #1205.

module M; record Pt(x: int, y: int); class C public static method M(p: Pt): int return (case p of new Pt(1, 2) => 10, _ => 0); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `case` pattern constant must be a literal, `null`, an enum constant, or a named `const` — a `new …` expression is not a valid pattern constant
// CHECK-SAME: (§A.7/§19.3)
