// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/scoped_static_owned_return_1606_test.cpp
//   (BEZEL_TEST scoped_static_owned_return_1606, static_scoped_no_owned_return_rejected).
//
// §28.14 (v0.180): a `scoped` parameter on a static method with NO `owned`
// return is rejected — no call site could ever resolve a bounding object.

module M; class C public static method Take(scoped f: () -> ()) f(); end Take; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `scoped` on parameter `f` of static method `Take` requires the method to have an `owned` return
// CHECK-SAME: (§28.14 v0.180)
