// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, inline_out_using_var_bang_accepted).
//
// (d'') inline `out using var` (OwningQueue.TryDequeue) infers `Cell?`; the
// `!`-narrowed field read is ACCEPTED (the using-var discharges the value).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _v: int := 0; public constructor(v: int) _v := v; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var q := new OwningQueue<Cell>(); q.Enqueue((new Cell(2))); var got := -1; if q.TryDequeue(out using var item) then got := item!._v; end; return got; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
