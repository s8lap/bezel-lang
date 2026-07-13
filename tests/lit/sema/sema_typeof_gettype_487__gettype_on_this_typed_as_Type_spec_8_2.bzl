// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, gettype_on_this_typed_as_Type_spec_8_2).
//
// §8.2: this.GetType() inside a class method → Type.

module M; class C public method M() var t := this.GetType(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'GetType()'
// CHECK-SAME: (§8.2, #487)
