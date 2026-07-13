// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_this_check_test.cpp
//   (BEZEL_TEST sema_static, this_in_method_field_access_on_static_diagnoses).

module M; class C public _x: int; public static method M() _ := this._x; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `this` is not available in a static method or static constructor — it requires an instance receiver
// CHECK-SAME: (§12.3)
