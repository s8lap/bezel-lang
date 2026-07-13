// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_this_check_test.cpp
//   (BEZEL_TEST sema_static, this_in_static_constructor_diagnoses).

module M; class C static constructor() _ := this; end constructor; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `this` is not available in a static method or static constructor — it requires an instance receiver
// CHECK-SAME: (§12.3)
