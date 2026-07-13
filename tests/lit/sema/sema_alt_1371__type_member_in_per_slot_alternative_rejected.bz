// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/pattern_alternatives_1371_test.cpp
//   (BEZEL_TEST sema_alt_1371, type_member_in_per_slot_alternative_rejected).

module M; class Animal end Animal; class Dog: Animal end Dog; record Box(a: Animal); class C public static method M(b: Box): int return case b of Box(Dog | Animal) => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a type may not appear in a `|` pattern alternative list — every member must be binding-free (a constant, a range, or a relational pattern); grouped type tests are written as separate arms
// CHECK-SAME: (§19.3)
