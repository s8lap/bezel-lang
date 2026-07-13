// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/channel_640_phantom_members_test.cpp
//   (BEZEL_TEST channel_640_phantom_members, channel_bogus_member_rejected_6_9).
//
// ---------------------------------------------------------------------------
// (3) §6.9 / #640: ch.Bogus() — entirely unknown member on Channel<T>.
// ---------------------------------------------------------------------------

module M; import System.Threading; class C public static method Test() using var ch := new Channel<int>(); ch.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `Channel<T>` (stdlib §6.9)
