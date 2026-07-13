// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, semaphore_try_acquire_inside_async_method_rejected_30_12).
//
// ---------------------------------------------------------------------------
// (e) Semaphore.TryAcquire(timeout) lexically inside an `async method` —
// rejected.
// ---------------------------------------------------------------------------

module M; class C public static async method Go(s: Semaphore): Task using var tok := s.TryAcquire(TimeSpan.FromSeconds(1)); end Go; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: use `await s.TryAcquireAsync(timeout)` — blocking Semaphore acquisition is not available in async code
// CHECK-SAME: (§30.12)
