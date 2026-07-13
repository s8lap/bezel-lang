// Ported from tests/sema/async_lambda_diagnostic_test.cpp
//   (BEZEL_TEST sema_async_lambda_30_6, async_lambda_with_await_accepted).
//
// §30.6 — an `async` lambda that awaits a Task<int> now type-checks with no
// error (the not-supported diagnostic is gone post-#488).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Producer(): Task<int> return 42; end Producer; public static async method Main(): Task<int> var f: () -> Task<int> := async () => await Producer(); using var t := f(); return await t; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
