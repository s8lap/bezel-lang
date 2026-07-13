// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/duplicate_top_level_type_747_test.cpp
//   (BEZEL_TEST duplicate_top_level_type_747, class_and_record_same_name_rejected).
//
// §11.3: a class and a record sharing a name are rejected (different kinds,
// same name — still a clash).

module M; class Bar public static V: int := 1; end Bar; record Bar(X: int); class P public static method Main(): int return 0; end Main; end P;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a top-level type named `Bar` is already declared in this module — top-level type names must be unique
// CHECK-SAME: (§11.3 well-formedness)
