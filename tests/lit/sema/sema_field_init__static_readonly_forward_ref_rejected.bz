// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, static_readonly_forward_ref_rejected).
//
// §22.13 (#747 R42): a static readonly field that reads a LATER-declared static
// readonly field of the same type is a compile error.

module M; class C public static readonly A: int := B + 1; public static readonly B: int := 5; public static method Main(): int return A; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static readonly `A` reads static readonly `B`, which is declared later in the same type — a forward reference between static readonly fields is not allowed; reorder the declarations or use a static constructor
// CHECK-SAME: (§22.13)
