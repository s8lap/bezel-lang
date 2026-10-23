// Ported from tests/sema/out_owned_if_branch_1407_test.cpp
//   (BEZEL_TEST out_owned_if_branch_1407, dispose_in_then_branch_code_after_accepted).
//
// §28.3 / #1407 (FALL-THROUGH + CODE AFTER): same as above but with additional
// code after the if-statement (another Push to verify the stack is usable).
// The join state after the if must be Disposed (not Owned), so there is no
// lingering obligation past the if — must ACCEPT.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal). It DOES emit one diagnostic — the `Cell._n` counter field below
// is a mutable static of primitive type, which §22.12 rule 2 form (iii)
// (#1393 warned-admission ruling, v0.221) admits only with a
// declaration-site warning, unrelated to this test's own #1407 subject —
// pinned below so the ACCEPT assertion stays accurate. The CHECK-NOT
// preserves this test's actual point: no §28.3 "auto-drop" warning on
// `item`.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); if s.TryPop(out var item) then item?.Dispose(); end; s.Push((new Cell())); end; return Cell._n; end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: auto-drop
// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_n` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii))
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
// CHECK-NOT: auto-drop
