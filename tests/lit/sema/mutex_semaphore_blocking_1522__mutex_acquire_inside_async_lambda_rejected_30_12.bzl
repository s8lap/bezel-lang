// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, mutex_acquire_inside_async_lambda_rejected_30_12).
//
// ---------------------------------------------------------------------------
// (c) Mutex.Acquire() lexically inside an `async` lambda — rejected.
// ---------------------------------------------------------------------------

module M; class C public static method Go(m: Mutex) var f := async () => do using var guard := m.Acquire(); end; end Go; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: use `await m.AcquireAsync()` — blocking Mutex acquisition is not available in async code
// CHECK-SAME: (§30.12)
