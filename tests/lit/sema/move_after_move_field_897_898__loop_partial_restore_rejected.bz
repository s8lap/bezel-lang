// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/move_after_move_field_897_898_test.cpp
//   (BEZEL_TEST move_after_move_field_897_898, loop_partial_restore_rejected).
//
// §28.8.1 / #898: a loop body moves the field but restores it only on the `c`
// path. On the c == false back-edge the field stays moved-from, so the next
// iteration's `move this._source` operates on an already-moved-from field. The
// path-sensitive fact OR-merged over the back-edge makes the move-site check
// fire. Must be REJECTED.

module M; class Resource public constructor() end constructor; destructor end destructor; end Resource; class Owner private owned _res: Resource; public constructor() _res := new Resource(); end constructor; public method Test(cond: bool, c: bool): int while cond do using var old := move this._res; if c then this._res := new Resource(); end; end; this._res := new Resource(); return 0; end Test; destructor end destructor; end Owner; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned field `_res` is already moved-from at this point; cannot move it again — restore it (`this._res := ...`) before moving again
// CHECK-SAME: (§28.8.1)
