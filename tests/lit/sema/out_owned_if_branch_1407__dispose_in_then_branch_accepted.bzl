// Ported from tests/sema/out_owned_if_branch_1407_test.cpp
//   (BEZEL_TEST out_owned_if_branch_1407, dispose_in_then_branch_accepted).
//
// §28.3 / #1407 (PRIMARY POSITIVE): `item?.Dispose()` in the then-branch of
// an if whose condition declares the item via `out owned var`. The false edge
// carries PolyNull (TryPop returned false → item not written); the then-branch
// exits Disposed; Disposed ⊓ PolyNull = Disposed → obligation satisfied.
// Must ACCEPT with no errors and no auto-drop warning.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell())); if s.TryPop(out owned var item) then item?.Dispose(); end; end; return Cell._n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
