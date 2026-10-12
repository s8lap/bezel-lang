// Issue #5312 / stdlib §1 (v0.313) + spec §22.4: `OwningStack<T>.TryPop` is a
// shape-(a) member, so `out owned var item` is narrowed non-null in the
// controlled `then` region and `item._v` is ACCEPTED without `!`. This is the
// program the (d') reject test used to pin as rejected before #5312.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _v: int := 0; public constructor(v: int) _v := v; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell(1))); if s.TryPop(out var item) then var v := item._v; item.Dispose(); return v; end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
