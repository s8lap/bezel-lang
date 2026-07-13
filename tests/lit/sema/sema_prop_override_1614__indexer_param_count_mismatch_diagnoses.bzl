// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, indexer_param_count_mismatch_diagnoses).
//
// §23 / §12.1 / #1614: an indexer override with a mismatched index-parameter
// COUNT is rejected.

module M; class Base public virtual property [i: int]: int => 0; end Base; class D : Base public override property [i: int, j: int]: int => 0; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: override of `indexer` does not match the base member's index parameter count
// CHECK-SAME: (§12.1)
