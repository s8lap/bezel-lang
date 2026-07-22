// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, range_scrutinee_char_const_pattern_rejected_spec_19_3_1399).
//
// §19.3 (#1399): char constant pattern against a Range scrutinee is also
// rejected — char is a scalar and Range is a struct.

module M; class C public static method Main(): int var r := 1..10; return (case r of 'a' => 0, _ => 1); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: constant pattern is not compatible with a Range-typed scrutinee — a Range value is a struct, not a scalar; use `_` or `var x` to match
// CHECK-SAME: (§19.3)
