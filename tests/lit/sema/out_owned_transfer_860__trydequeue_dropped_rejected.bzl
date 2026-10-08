// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trydequeue_dropped_rejected).
//
// §28.3 / §28.11 / #860 / #1387: OwningQueue.TryDequeue — out-owned at arg 0 —
// dropped without disposal. As with TryPop (#1387 affine auto-drop), this is now
// a must-use WARNING with scope-end disposal, not a hard error.  Confirms the
// `item` is still marked Owned (the #860 mark-owned fix intact) so the must-use
// path fires, not silent acceptance (the pre-#860 bug).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var q := new OwningQueue<Cell>(); q.Enqueue((new Cell(1))); var ok := q.TryDequeue(out var item); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
