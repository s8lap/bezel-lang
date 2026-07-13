// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, statement_case_out_of_range_rejected_spec_5_1).
//
// Statement-position `case` is exempt from the exhaustiveness WARNING, but the
// literal-range gate still applies (codegen would otherwise truncate). #1153.

module M; class C public static method M(x: byte) case x of 256 => do return; end, _ => do return; end end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `256` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
