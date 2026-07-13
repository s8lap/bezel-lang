// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, implements_idisposable_diagnoses).

module M; record R : IDisposable public _x: int := 0; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` may not implement `IDisposable`
// CHECK-SAME: (§9.7)
