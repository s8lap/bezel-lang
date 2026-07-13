// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, gettype_on_record_typed_as_Type_spec_8_2).
//
// §8.2: GetType() on a record-typed receiver → Type.

module M; record Point(x: int, y: int); class C public method M(p: Point) var t := p.GetType(); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'GetType()'
// CHECK-SAME: (§8.2, #487)
