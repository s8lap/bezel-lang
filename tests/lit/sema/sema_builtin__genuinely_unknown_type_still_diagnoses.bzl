// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_types_test.cpp
//   (BEZEL_TEST sema_builtin, genuinely_unknown_type_still_diagnoses).

module M; class C public _x: NotARealType; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved type name `NotARealType`
