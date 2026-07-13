// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, try_receive_out_owned_shape_accepted_30_15).
//
// §30.15 stdlib §6.9: TryReceive(out owned item: T?) shape for class T.
// `out using var` at the call site.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Packet public Len: int := 0; destructor end destructor; end Packet; class C public static method Test() using var ch := new Channel<Packet>(); var got := ch.TryReceive(out using var item); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
