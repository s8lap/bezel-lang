// Ported from tests/sema/mutex_sendable_647_test.cpp
//   (BEZEL_TEST mutex_sendable_647, channel_mutex_accepted_6_2_v0138_30_15).
//
// §30.15 / §3.6 / stdlib §6.2 v0.138: Channel<Mutex> accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var ch := new Channel<Mutex>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
