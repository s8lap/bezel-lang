// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/channel_640_phantom_members_test.cpp
//   (BEZEL_TEST channel_640_phantom_members, channel_send_on_receiver_end_rejected_6_9).
//
// ---------------------------------------------------------------------------
// (1) §6.9 / #640: ch.Send(42) on Channel<int> → compile error.
// Send is on ChannelSender<T>, not Channel<T>.
// ---------------------------------------------------------------------------

module M; import System.Threading; class C public static method Test() using var ch := new Channel<int>(); ch.Send(42); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Send` on `Channel<T>`; `Send` is only available on `ChannelSender<T>` — obtain one via `ch.NewSender()` (stdlib §6.9, #640) (stdlib §6.9)
