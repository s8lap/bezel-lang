// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, send_receive_class_ownership_shapes_accepted_30_15).
//
// §30.15 stdlib §6.9: for class-typed T, Send takes `owned item: T` and
// Receive() returns OwningTask<T>. The pattern below must type-check.
// Channel-in-receive-domain pattern from the spec example.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Report public Id: int := 0; destructor end destructor; end Report; class Receiver public static async method RunReceiver(owned ch: Channel<Report>): Task using var inbox := ch; using var item := await inbox.Receive(); end RunReceiver; end Receiver;

// NODIAG-NOT: error
// NODIAG-NOT: §
