// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/duplicate_member_test.cpp
//   (BEZEL_TEST sema_dup, duplicate_in_nested_type_diagnose).

module M; class Outer public class Inner public _z: int; public _z: int; end Inner; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: duplicate member `_z` in type `Inner` (a non-overloadable member cannot share a name with another member of the same type; §11.3 lists the member kinds — uniqueness is a well-formedness requirement)
