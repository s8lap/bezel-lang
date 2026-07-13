// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, recursive_value_record_inline_array_rejected_1272).
//
// An inline array of the record itself is still an inline value cycle.

module M; record Tree(kids: [3]Tree, v: int);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Tree` cannot recursively contain itself by value — a record is stored inline, so a self-referential record has infinite size (a nullable `Tree?` field is still stored inline); use a `class` (a heap reference) for a recursive structure
// CHECK-SAME: (§9)
