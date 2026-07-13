// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, destructor_diagnoses).

module M; record R public _x: int := 0; destructor end destructor; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` may not declare a destructor
// CHECK-SAME: (§9.7)
