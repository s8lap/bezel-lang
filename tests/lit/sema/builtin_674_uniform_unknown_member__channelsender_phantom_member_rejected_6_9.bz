// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/builtin_674_uniform_unknown_member_test.cpp
//   (BEZEL_TEST builtin_674_uniform_unknown_member, channelsender_phantom_member_rejected_6_9).
//
// (a) Phantom member on ChannelSender<T> → compile error.

module M; import System.Threading; class C public static method Test() using var ch := new Channel<int>(); using var sender := ch.NewSender(); sender.Bogus(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `Bogus` on `ChannelSender<T>`; documented surface: Send(owned item: T), SendAsync(owned item: T): Task, Dispose() (stdlib §6.9)
