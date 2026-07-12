// Ported from tests/ownership/nested_new_owned_arg_835_test.cpp
//   (BEZEL_TEST nested_new_owned_arg_835, nested_new_fresh_producer_accepted).
//
// Valid: a fresh `new Cell(...)` (an owned producer) nested through the chain is
// accepted — the transfer is real, not a borrow.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public val: int; public constructor(v: int) val := v; end constructor; destructor end destructor; end Cell; class Box private owned _c: Cell; public constructor(owned v: Cell) _c := v; end constructor; destructor end destructor; end Box; class Outer private owned _b: Box; public constructor(owned v: Box) _b := v; end constructor; destructor end destructor; end Outer; class C public static method Mk(): owned Outer return new Outer(new Box(new Cell(5))); end Mk; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
