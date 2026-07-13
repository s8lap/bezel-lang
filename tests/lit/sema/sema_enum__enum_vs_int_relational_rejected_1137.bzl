// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, enum_vs_int_relational_rejected_1137).
//
// Relational enum-vs-int is also rejected (#1137).

module M; enum Color Red, Green; end Color; class C public static method Main(): int if Color.Red < 1 then return 1; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<` compares enum `Color` with a non-enum operand — an enum may be compared only with a value of the same enum type
// CHECK-SAME: (§8.4 / §10)
