// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, stmt_form_860_regression_dispose_accepted).
//
// §28.3 / §28.11 / #860 (REGRESSION — statement form dispose): disposing after
// a statement-level TryPop still ACCEPTS. Guards the #860 positive case.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell())); s.TryPop(out owned var item); item?.Dispose(); end; return Cell._n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
