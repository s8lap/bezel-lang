// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, new_semaphore_token_rejected_6_3).
//
// §6.3: `new SemaphoreToken()` is a compile error.

module M; class C public static method Test() using var tok := new SemaphoreToken(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `SemaphoreToken` has no public constructor: obtain a SemaphoreToken via `semaphore.Acquire()` or `semaphore.TryAcquire(timeout)` (stdlib §6.3, #647)
