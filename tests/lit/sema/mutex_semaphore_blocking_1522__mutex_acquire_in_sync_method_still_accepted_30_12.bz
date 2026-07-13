// Ported from tests/sema/mutex_semaphore_blocking_1522_test.cpp
//   (BEZEL_TEST mutex_semaphore_blocking_1522, mutex_acquire_in_sync_method_still_accepted_30_12).
//
// ---------------------------------------------------------------------------
// (f) Mutex.Acquire()/TryAcquire() in a plain (non-async) method — STILL
// ACCEPTED. Stdlib §6.2/§6.3: "They remain fully legal on the startup
// thread ... and on FFI foreign threads (outside any domain)."
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Go(m: Mutex) using var guard := m.Acquire(); using var maybe := m.TryAcquire(TimeSpan.FromSeconds(1)); end Go; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
