// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, concrete_method_without_body_diagnoses).

module M; class C public method F(): int; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: concrete class `C` does not implement inherited abstract member `F`
// CHECK-SAME: (§12.5)
