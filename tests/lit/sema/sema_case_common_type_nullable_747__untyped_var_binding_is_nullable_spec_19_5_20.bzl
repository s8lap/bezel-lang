// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/case_common_type_nullable_747_test.cpp
//   (BEZEL_TEST sema_case_common_type_nullable_747, untyped_var_binding_is_nullable_spec_19_5_20).
//
// §19.5 / §20: untyped `var n` over an `int?` scrutinee binds the whole value;
// the case's common type is `int?`. Returning it from an `int`-returning method
// is a §7 nullable-into-non-nullable error — NOT a compiler crash.

module M; class C public static method Main(): int var x: int? := 1; return (case x of null => 0, var n => n); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the returned value is nullable but the target type is non-nullable
// CHECK-SAME: (§7)
