// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, enum_vs_different_enum_comparison_rejected_1137).
//
// enum compared with a DIFFERENT enum is rejected (#1137).

module M; enum Color Red, Green; end Color; enum Size Small, Big; end Size; class C public static method Main(): int if Color.Red = Size.Small then return 1; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `=` compares enum `Color` with a different enum `Size` — an enum may be compared only with a value of the same enum type
// CHECK-SAME: (§8.4 / §10)
