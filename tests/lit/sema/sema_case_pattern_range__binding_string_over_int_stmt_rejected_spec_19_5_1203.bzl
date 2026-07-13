// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_string_over_int_stmt_rejected_spec_19_5_1203).
//
// §19.5 (#1203): statement-position `case` also rejects the incompatible
// binding — the CaseStmt handler has the same check.

module M; class C public static method M(x: int) case x of var s: string => do return; end end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: binding pattern type is incompatible with the scrutinee's static type — a value-type scrutinee can never match a reference-typed binding
// CHECK-SAME: (§19.5)
