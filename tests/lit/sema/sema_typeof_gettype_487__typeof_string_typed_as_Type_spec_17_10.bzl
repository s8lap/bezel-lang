// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, typeof_string_typed_as_Type_spec_17_10).
//
// §17.10: typeof(string) → Type.  Another primitive.

module M; class C public method M() var t := typeof(string); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'typeof'
// CHECK-SAME: (§17.10, #487)
