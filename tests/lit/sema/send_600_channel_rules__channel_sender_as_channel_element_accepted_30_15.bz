// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, channel_sender_as_channel_element_accepted_30_15).
//
// §30.15 / stdlib §6.9: ChannelSender<T> can itself be a Channel element
// (sendable object through a sendable channel). This is an advanced pattern
// used for dynamic wiring.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Report public Id: int := 0; destructor end destructor; end Report; class C public static method Test() using var ch := new Channel<ChannelSender<Report>>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
