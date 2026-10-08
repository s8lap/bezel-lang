// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypopvalue_pos1_dropped_rejected).
//
// §28.3 / §28.11 / #860 / #1387: OwningDictionary.TryPopValue(key, out var
// value) — out-owned at arg POSITION 1 (not 0) — dropped without disposal.
// Under #1387 affine auto-drop: must-use WARNING, not a hard error. Guards the
// per-method argument-index table AND confirms `v` is still marked Owned (the
// #860 mark-owned fix, not the pre-#860 silent-acceptance bug).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var d := new OwningDictionary<string, Cell>(); d.Add("a", (new Cell(1))); var ok := d.TryPopValue("a", out var v); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `v` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
