// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, unknown_method_rejected_6_3).
//
// §6.3 / #674: an unknown method on Semaphore produces a diagnostic.

module M; class C public static method Test() using var sem := new Semaphore(1); sem.Release(99); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Release` on `Semaphore`; documented surface: Acquire(), TryAcquire(timeout: TimeSpan), AcquireAsync(), TryAcquireAsync(), Dispose() (stdlib §6.3)
