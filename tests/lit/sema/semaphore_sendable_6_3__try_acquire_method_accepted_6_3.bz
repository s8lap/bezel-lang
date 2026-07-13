// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, try_acquire_method_accepted_6_3).
//
// §6.3: TryAcquire(timeout) is a valid method on Semaphore — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var sem := new Semaphore(2); using var tok := sem.TryAcquire(TimeSpan.FromSeconds(1)); if tok <> null then end; end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
