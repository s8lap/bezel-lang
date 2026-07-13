// Ported from tests/ownership/inline_owned_array_slot_sink_780_test.cpp
//   (BEZEL_TEST inline_owned_array_slot_sink_780, null_into_nullable_slot_accepted_28_11).
//
// §28.11: assigning `null` to a nullable owning slot is legal (it clears the
// slot); it is not a borrow store. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public n: int := 0; end Cell; class C public static method Main(): int do var a: [2 owned]Cell? := [new Cell(), new Cell()]; a[0] := null; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
