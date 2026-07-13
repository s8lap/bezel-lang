// Ported from tests/ownership/inline_owned_array_slot_sink_780_test.cpp
//   (BEZEL_TEST inline_owned_array_slot_sink_780, new_into_slot_accepted_28_11).
//
// §28.11: a fresh `new Cell()` is an owned producer — the canonical owned value
// for an owned slot. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public n: int := 0; end Cell; class C public static method Main(): int do var a: [2 owned]Cell? := [new Cell(), new Cell()]; a[0] := new Cell(); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
