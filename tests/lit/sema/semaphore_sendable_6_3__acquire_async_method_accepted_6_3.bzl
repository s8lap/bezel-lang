// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, acquire_async_method_accepted_6_3).
//
// §6.3: AcquireAsync() is valid on Semaphore in an async method — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Go(): Task using var sem := new Semaphore(1); using var tok := await sem.AcquireAsync(); end Go; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
