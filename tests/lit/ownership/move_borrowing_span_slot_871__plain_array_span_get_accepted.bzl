// Ported from tests/ownership/move_borrowing_span_slot_871_test.cpp
//   (BEZEL_TEST move_borrowing_span_slot_871, plain_array_span_get_accepted).
//
// §28.11 / §4.15: a plain indexer-get on an `ArraySpan<T>` (no `move`) reads the
// borrow and is unaffected by this fix — still accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero.
//
// The setup store was `arr.Add(move c1);` — a `move` result is one of §15.4's
// fresh forms, so that store drew the §15.4 fresh-store warning (fix #3364,
// v0.243) and, since v0.346 (proposal 0207, #3047 lint A), is a compile
// ERROR: "a fresh unbound owned temporary of class type … in a single-element
// store position of a borrowing-family collection is a compile error at the
// store site" (§15.4, v0.355). The setup is re-cut to §15.4's own remedy —
// bind the value to a named holder first and store the holder (`using var c1
// := new Cell(); arr.Add(c1);`, a same-scope local the §28.11 store rule
// admits) — so the program stays a clean-compile ACCEPT control for the
// property this test pins: the plain span get.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public V: int := 0; end Cell; class C public static method Main(): int const arr: Array<Cell> := []; using var c1 := new Cell(); arr.Add(c1); var sp := arr.AsSpan(); var v := sp[0].V; return v; end Main; end C;

// NODIAG-NOT: {{error|warning}}
