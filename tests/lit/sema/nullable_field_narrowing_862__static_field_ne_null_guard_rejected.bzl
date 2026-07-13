// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, static_field_ne_null_guard_rejected).
//
// §7.4 (#862): a static nullable field is likewise un-narrowable.

module M; class Counter public static _label: string? := "hello"; public static method Main(): int if Counter._label <> null then return Counter._label.Length; end; return 0; end Main; end Counter;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)
