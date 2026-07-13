// Ported from tests/sema/move_after_move_field_897_898_test.cpp
//   (BEZEL_TEST move_after_move_field_897_898, move_restore_move_bare_accepted).
//
// §28.8.1 / #897 #898 (over-reject guard, bare form): the bare `move _res`
// move-restore-move must be accepted identically.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Resource public constructor() end constructor; destructor end destructor; end Resource; class Owner private owned _res: Resource; public constructor() _res := new Resource(); end constructor; public method MRM(): int using var a := move _res; this._res := new Resource(); using var b := move _res; this._res := new Resource(); return 0; end MRM; destructor end destructor; end Owner; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
