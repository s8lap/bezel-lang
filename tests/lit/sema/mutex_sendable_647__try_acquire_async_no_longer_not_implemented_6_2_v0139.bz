// Ported from tests/sema/mutex_sendable_647_test.cpp
//   (BEZEL_TEST mutex_sendable_647, try_acquire_async_no_longer_not_implemented_6_2_v0139).
//
// §6.2 / #647 / v0.139: TryAcquireAsync in an async method produces no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Go(): Task using var mutex := new Mutex(); using var guard := await mutex.TryAcquireAsync( TimeSpan.FromSeconds(5)); end Go; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
