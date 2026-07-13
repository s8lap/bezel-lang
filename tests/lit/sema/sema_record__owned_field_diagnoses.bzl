// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, owned_field_diagnoses).

module M; class T end T; record R public owned _t: T; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` may not have an `owned` field `_t`
// CHECK-SAME: (§9.7)
