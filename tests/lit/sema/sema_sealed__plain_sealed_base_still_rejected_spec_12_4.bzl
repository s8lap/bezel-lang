// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, plain_sealed_base_still_rejected_spec_12_4).
//
// §12.4 regression guard: a plain `sealed` (non-abstract) base must STILL be
// rejected as a base, even same-module. The fix for §11.7 special-cases only
// `closed`; plain sealing is unchanged.

module M; sealed class Base public method V(): int => 1; end Base; class Derived : Base end Derived;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot inherit from sealed class `Base`
// CHECK-SAME: (§12.4)
