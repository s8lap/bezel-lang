// Ported from tests/sema/out_owned_if_branch_1407_test.cpp
//   (BEZEL_TEST out_owned_if_branch_1407, other_work_in_then_no_dispose_still_warned).
//
// §28.3 / #1407 (NEGATIVE GUARDRAIL — multi-element then-body, no disposal):
// the then-branch does other work (pushes another element) but never discharges
// item. Must still warn — the obligation carries through regardless.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); if s.TryPop(out owned var item) then s.Push((new Cell())); end; end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
