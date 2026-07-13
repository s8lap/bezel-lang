// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, class_user_dispose_diagnoses).
//
// §28.2: "User code does not implement Dispose() directly."

module M; class Resource public method Dispose() _ := 0; end Dispose; end Resource;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Resource` may not declare a `Dispose()` method directly
// CHECK-SAME: (§28.2)
