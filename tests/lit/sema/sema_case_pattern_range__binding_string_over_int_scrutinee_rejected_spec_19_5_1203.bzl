// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_string_over_int_scrutinee_rejected_spec_19_5_1203).
//
// §19.5 (#1203): `var s: string` over an `int` scrutinee — a value-type
// primitive can never be a string (a reference type). Was previously compiled
// as an unconditional match (rc=1 instead of rc=2), leading to wrong-value
// results and IR_FAIL when the bound `s` was used as a string.

module M; class C public static method M(x: int): int return (case x of var s: string => 1, _ => 2); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: binding pattern type is incompatible with the scrutinee's static type — a value-type scrutinee can never match a reference-typed binding
// CHECK-SAME: (§19.5)
