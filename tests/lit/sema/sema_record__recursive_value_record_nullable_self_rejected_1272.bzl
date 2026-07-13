// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, recursive_value_record_nullable_self_rejected_1272).
//
// Direct self-reference through a NULLABLE field (`Node?` is still stored inline).

module M; record Node(Val: int, Next: Node?);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Node` cannot recursively contain itself by value — a record is stored inline, so a self-referential record has infinite size (a nullable `Node?` field is still stored inline); use a `class` (a heap reference) for a recursive structure
// CHECK-SAME: (§9)
