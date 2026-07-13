// Ported from tests/sema/shared_sync_sendable_689_test.cpp
//   (BEZEL_TEST shared_sync_sendable_689, channel_shared_mutex_accepted_28_13_v0140_30_15).
//
// §30.15 / §3.6 / §28.13 v0.140: Channel<Shared<Mutex>> accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var ch := new Channel<Shared<Mutex>>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
