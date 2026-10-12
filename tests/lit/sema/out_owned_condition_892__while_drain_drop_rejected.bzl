// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, while_drain_drop_rejected).
//
// §28.3 / #892 (WHILE DRAIN REJECT): item dropped without disposal in the
// while body → REJECT (obligation incurred by the condition, not discharged).

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell())); while s.TryPop(out var item) do // item dropped without disposal end; end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected 'end' to close 'while' statement
