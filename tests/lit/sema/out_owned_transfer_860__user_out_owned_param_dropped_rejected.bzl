// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, user_out_owned_param_dropped_rejected).
//
// §28.3 / §28.11 / #860 / #1387 (user method): a USER method's `out owned r`
// parameter is the owned-OUT direction; the caller's `out var` adopts owned. The receiving local dropped without
// disposal. Under #1387 affine auto-drop: must-use WARNING, not a hard error.
// Before the #860 fix, the `out owned` formal was mis-classified as an owned-IN
// sink — now it correctly receives and marks `got` Owned. Before #1387 that
// generated a hard error; now it generates the auto-drop warning.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class Box private owned _c: Cell; public constructor() _c := move (new Cell(1)); end constructor; public method TakeIt(out owned r: Cell): bool r := move _c; _c := move (new Cell(2)); return true; end TakeIt; destructor end destructor; end Box; class C public static method Main(): int do using var b := new Box(); var ok := b.TakeIt(out var got); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `got` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
