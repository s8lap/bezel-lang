// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/pattern_alternatives_1371_test.cpp
//   (BEZEL_TEST sema_alt_1371, binding_member_in_alternative_rejected).

module M; class C public static method M(x: int): int return case x of 1 | var y => 1, _ => 0 end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a pattern alternative member must be a constant — a literal, an enum constant, or a named `const`
// CHECK-SAME: (§19.3)
