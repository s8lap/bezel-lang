// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, gettype_not_silently_untyped_spec_8_2).
//
// GetType() TypeRef must be non-null (no silent garbage/crash path).

module M; class Animal public _name: string; end Animal; class C public method M(a: Animal) var t := a.GetType(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'GetType()'
// CHECK-SAME: (§8.2, #487)
