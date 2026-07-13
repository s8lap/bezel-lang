// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, incomplete_comparison_group_rejected_on_enum_spec_25_2).
//
// §25.2: "Comparison operators must be overloaded as a complete group."
// declaring only `<` on an enum must be rejected exactly as on a record.

module M; enum Priority Low, Medium, High; public static method operator <(a: Priority, b: Priority): bool => int(a) < int(b); end Priority; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: comparison operators must be overloaded as a complete group: `<`, `>`, `<=`, `>=` together
// CHECK-SAME: (§25.2)
