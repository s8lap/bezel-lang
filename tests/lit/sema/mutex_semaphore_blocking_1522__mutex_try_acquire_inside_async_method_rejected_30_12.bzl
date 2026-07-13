// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, mutex_try_acquire_inside_async_method_rejected_30_12).
//
// ---------------------------------------------------------------------------
// (b) Mutex.TryAcquire(timeout) lexically inside an `async method` — rejected.
// ---------------------------------------------------------------------------

module M; class C public static async method Go(m: Mutex): Task using var guard := m.TryAcquire(TimeSpan.FromSeconds(1)); end Go; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: use `await m.TryAcquireAsync(timeout)` — blocking Mutex acquisition is not available in async code
// CHECK-SAME: (§30.12)
