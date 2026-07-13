// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_lambda_30_6, await_in_non_async_lambda_rejected).
//
// §30.2 — `await` in a NON-async lambda is still rejected (the lambda's own
// async-ness, not the enclosing method's, governs await validity). This pins
// that removing the not-supported diagnostic did not relax the await boundary.

module M; class C public static async method Producer(): Task<int> return 42; end Producer; public static async method Main(): Task<int> var f: () -> int := () => await Producer(); _ := f; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `await` is valid only inside an `async` method or `async` lambda
// CHECK-SAME: (§30.2)
