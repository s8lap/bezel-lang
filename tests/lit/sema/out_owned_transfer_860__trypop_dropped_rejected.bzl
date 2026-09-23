// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypop_dropped_rejected).
//
// §28.3 / §28.11 / #860 / #1387: an `out owned var item` inline local moved out
// of an OwningStack via TryPop and then dropped without disposal. Under #1387 the
// §28.3 v0.150 affine auto-drop rule applies to ALL `out owned var` inline fresh
// locals (statement-level and if-condition alike): emit a must-use WARNING and
// register scope-end disposal rather than a hard error.  Before #1387 this was a
// hard error; before #860 it was silently accepted (a leak).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell(1))); var ok := s.TryPop(out owned var item); end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `item` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)
