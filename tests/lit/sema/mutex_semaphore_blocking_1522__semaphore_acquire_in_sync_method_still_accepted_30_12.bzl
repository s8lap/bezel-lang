// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, semaphore_acquire_in_sync_method_still_accepted_30_12).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Go(s: Semaphore) using var tok := s.Acquire(); using var maybe := s.TryAcquire(TimeSpan.FromSeconds(1)); end Go; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
