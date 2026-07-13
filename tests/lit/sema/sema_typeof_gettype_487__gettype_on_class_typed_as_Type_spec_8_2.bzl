// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, gettype_on_class_typed_as_Type_spec_8_2).
//
// §8.2: obj.GetType() on a class-typed receiver → Type.

module M; class Widget public _id: int; end Widget; class C public method M(w: Widget) var t := w.GetType(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'GetType()'
// CHECK-SAME: (§8.2, #487)
