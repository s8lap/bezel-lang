// Ported from tests/ownership/inline_owned_array_slot_sink_780_test.cpp
//   (BEZEL_TEST inline_owned_array_slot_sink_780, owned_local_transfer_into_slot_accepted_28_3).
//
// §28.3: a bare owned-LOCAL read is a move-on-read INTO the owned slot — a legal
// transfer that leaves the local moved-from. Previously FALSE-REJECTED as
// "owned value … dropped without disposal" because the slot write was not seen
// as a transfer. Now ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public n: int := 0; end Cell; class C public static method Main(): int do var a: [2 owned]Cell? := [new Cell(), new Cell()]; var c := new Cell(); a[0] := c; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
