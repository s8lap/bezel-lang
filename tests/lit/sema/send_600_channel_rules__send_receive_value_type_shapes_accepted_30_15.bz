// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, send_receive_value_type_shapes_accepted_30_15).
//
// §30.15 stdlib §6.9: for value-typed T, Send(item: T) and Receive():Task<T>.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method Test(): Task using var ch := new Channel<int>(); using var tx := ch.NewSender(); tx.Send(42); var n := await ch.Receive(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
