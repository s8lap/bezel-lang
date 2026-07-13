// Ported from tests/sema/move_after_move_field_897_898_test.cpp
//   (BEZEL_TEST move_after_move_field_897_898, loop_unconditional_restore_accepted).
//
// §28.8.1 / #898 (over-reject guard): a loop that moves AND UNCONDITIONALLY
// restores the field each iteration — the field is restored on every back-edge
// path, so the next iteration's move is valid. Must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Resource public constructor() end constructor; destructor end destructor; end Resource; class Owner private owned _res: Resource; public constructor() _res := new Resource(); end constructor; public method Loop(cond: bool): int while cond do using var o := move this._res; this._res := new Resource(); end; return 0; end Loop; destructor end destructor; end Owner; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
