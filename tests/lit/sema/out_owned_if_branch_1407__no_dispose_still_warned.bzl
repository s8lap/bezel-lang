// Ported from tests/sema/out_owned_if_branch_1407_test.cpp
//   (BEZEL_TEST out_owned_if_branch_1407, no_dispose_still_warned).
//
// §28.3 / #1407 (NEGATIVE GUARDRAIL — no disposal): item received in the
// then-branch is NOT disposed. The then-branch exits Owned, false-edge is
// PolyNull; meet = Owned (PolyNull absorbs into Owned, keeping Owned).
// The obligation surfaces: auto-drop warning expected.
// Guards that the fix does NOT over-relax genuine leaks.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); if s.TryPop(out owned var item) then end; end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
