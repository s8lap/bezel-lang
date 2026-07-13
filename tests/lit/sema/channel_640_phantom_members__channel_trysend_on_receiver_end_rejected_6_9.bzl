// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/channel_640_phantom_members_test.cpp
//   (BEZEL_TEST channel_640_phantom_members, channel_trysend_on_receiver_end_rejected_6_9).
//
// ---------------------------------------------------------------------------
// (2) §6.9 / #640: ch.TrySend(42) on Channel<int> → compile error.
// TrySend does not exist on Channel<T> or ChannelSender<T> (§6.9).
// ---------------------------------------------------------------------------

module M; import System.Threading; class C public static method Test() using var ch := new Channel<int>(); ch.TrySend(42); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `TrySend` on `Channel<T>`; `TrySend` is only available on `ChannelSender<T>` — obtain one via `ch.NewSender()` (stdlib §6.9, #640) (stdlib §6.9)
