// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/duplicate_top_level_type_747_test.cpp
//   (BEZEL_TEST duplicate_top_level_type_747, two_classes_same_name_rejected).
//
// §11.3: two top-level classes with the same name are rejected.

module M; class Foo public static Val: int := 1; end Foo; class Foo public static Val: int := 2; end Foo; class P public static method Main(): int return Foo.Val; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a top-level type named `Foo` is already declared in this module — top-level type names must be unique
// CHECK-SAME: (§11.3 well-formedness)
