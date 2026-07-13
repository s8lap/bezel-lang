// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/typeof_gettype_487_test.cpp
//   (BEZEL_TEST sema_typeof_gettype_487, typeof_not_silently_untyped_spec_17_10).
//
// typeof(bool) TypeRef must be non-null (no silent garbage/crash path).

module M; class C public method M() var t := typeof(bool); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: reflection is not yet implemented: 'typeof'
// CHECK-SAME: (§17.10, #487)
