// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, user_out_owned_param_dispose_accepted).
//
// §28.3 / §28.11 / #860 (user method): the same user `out owned` form with a
// null-safe dispose of the receiving local — ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class Box private owned _c: Cell; public constructor() _c := move (new Cell(1)); end constructor; public method TakeIt(out owned r: Cell): bool r := move _c; _c := move (new Cell(2)); return true; end TakeIt; destructor end destructor; end Box; class C public static method Main(): int do using var b := new Box(); var ok := b.TakeIt(out var got); got?.Dispose(); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
