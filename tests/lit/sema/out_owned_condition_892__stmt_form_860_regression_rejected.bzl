// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, stmt_form_860_regression_rejected).
//
// §28.3 / §28.11 / #860 / #1387 (REGRESSION — statement form): the statement-
// level `s.TryPop(out owned var item)` with an undischarged item now follows
// the same affine auto-drop rule as the if-condition form (#1387): it emits a
// must-use WARNING and registers scope-end disposal, not a hard error. The
// item is still correctly marked Owned (the #860 mark-owned fix is intact).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); s.TryPop(out owned var item); end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
