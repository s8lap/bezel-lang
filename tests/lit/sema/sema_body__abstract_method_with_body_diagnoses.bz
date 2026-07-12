// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, abstract_method_with_body_diagnoses).

module M; abstract class C public abstract method F(): int => 1; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: abstract method `F` may not have a body
// CHECK-SAME: (§12.5)
