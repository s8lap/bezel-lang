// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_this_check_test.cpp
//   (BEZEL_TEST sema_static, base_in_static_method_diagnoses).

module M; class B end B; class C : B public static method M() _ := base; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `base` is not available in a static method or static constructor — it requires an instance receiver
// CHECK-SAME: (§12.3)
