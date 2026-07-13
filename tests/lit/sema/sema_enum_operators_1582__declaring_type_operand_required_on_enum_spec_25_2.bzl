// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, declaring_type_operand_required_on_enum_spec_25_2).
//
// §25.2: "At least one parameter of an operator overload must be the
// declaring type." This restriction is stated type-kind-agnostically and
// must reach an enum's own declarations exactly as it reaches a record's.

module M; enum Priority Low, Medium, High; public static method operator +(a: int, b: int): int => a + b; end Priority; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: at least one parameter of operator `+` must be the declaring type `Priority` — an operator with no declaring-type parameter is unreachable by the resolution rule
// CHECK-SAME: (§25.2)
