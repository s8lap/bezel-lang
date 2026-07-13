// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, const_member_non_primitive_rejected_1564_22_12).
//
// A class-typed enum const is rejected — §22.12's "primitive or `string`"
// restriction, same as on a class.

module M; class Widget end Widget; enum Kind A, B; public const Bad: Widget := new Widget(); end Kind;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: const `Bad` must have a primitive value type or `string`
// CHECK-SAME: (§22.12)
