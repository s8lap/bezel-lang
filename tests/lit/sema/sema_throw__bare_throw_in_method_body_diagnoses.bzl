// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bare_throw_test.cpp
//   (BEZEL_TEST sema_throw, bare_throw_in_method_body_diagnoses).

module M; class C public method M(flag: bool) throw; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: bare `throw;` (re-raise) is only valid inside a catch arm body
// CHECK-SAME: (§31.5)
