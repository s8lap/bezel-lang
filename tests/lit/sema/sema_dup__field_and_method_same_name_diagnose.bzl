// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/duplicate_member_test.cpp
//   (BEZEL_TEST sema_dup, field_and_method_same_name_diagnose).

module M; class C public Value: int; public method Value() _ := 1; end Value; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: duplicate member `Value` in type `C` (a non-overloadable member cannot share a name with another member of the same type; §11.3 lists the member kinds — uniqueness is a well-formedness requirement)
