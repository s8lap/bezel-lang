// Ported from tests/sema/semaphore_sendable_647_test.cpp
//   (BEZEL_TEST semaphore_sendable_6_3, channel_semaphore_accepted_6_3_30_15).
//
// §30.15 / §3.6 / stdlib §6.3: Channel<Semaphore> accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var ch := new Channel<Semaphore>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
