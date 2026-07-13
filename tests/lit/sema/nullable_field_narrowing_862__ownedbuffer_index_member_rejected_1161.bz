// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, ownedbuffer_index_member_rejected_1161).
//
// §7.4 (#1161): direct `.member` on an `OwningBuffer<classT>` index result
// (typed `Cell?` per #1157) is rejected — narrow to non-null first.

module M; class Cell public v: int; public constructor(x: int) this.v := x; end constructor; end Cell; class C public static method Main(): int using var buf := new OwningBuffer<Cell>(2); return buf[0].v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.v` on a nullable index result requires narrowing to non-null first
// CHECK-SAME: (§7.4)
