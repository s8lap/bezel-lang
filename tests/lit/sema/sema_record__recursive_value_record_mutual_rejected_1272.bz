// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/record_restrictions_test.cpp
//   (BEZEL_TEST sema_record, recursive_value_record_mutual_rejected_1272).
//
// Mutual recursion A→B→A.

module M; record A(b: B?, x: int); record B(a: A?, y: int);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `A` cannot recursively contain itself by value — a record is stored inline, so a self-referential record has infinite size (a nullable `A?` field is still stored inline); use a `class` (a heap reference) for a recursive structure
// CHECK-SAME: (§9)
