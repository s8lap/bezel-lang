// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/duplicate_member_test.cpp
//   (BEZEL_TEST sema_dup, duplicate_fields_diagnose).

module M; class C public _x: int; public _x: int; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: duplicate member `_x` in type `C` (a non-overloadable member cannot share a name with another member of the same type; §11.3 lists the member kinds — uniqueness is a well-formedness requirement)
