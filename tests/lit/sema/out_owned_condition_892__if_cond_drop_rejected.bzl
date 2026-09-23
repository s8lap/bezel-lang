// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, if_cond_drop_rejected).
//
// §28.3 / #892 / #1387: `if s.TryPop(out owned var item) then end;` — item
// received ownership in the condition and is never discharged. Under #1387 the
// affine auto-drop rule (§28.3 v0.150) now applies to `out owned var` inline
// locals that are never discharged: emit a MUST-USE WARNING (not an error) and
// register scope-end disposal. Before #1387 this was a hard error.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); if s.TryPop(out owned var item) then end; end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
