// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, mutex_acquire_async_inside_async_method_still_accepted_30_12).
//
// ---------------------------------------------------------------------------
// (g) Mutex.AcquireAsync()/TryAcquireAsync() lexically inside an `async
// method` — accepted (the async forms are exactly what §30.12 asks for
// in place of the blocking forms; must not be caught by the new rule).
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Go(m: Mutex): Task using var guard := await m.AcquireAsync(); using var maybe := await m.TryAcquireAsync(TimeSpan.FromSeconds(1)); end Go; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
