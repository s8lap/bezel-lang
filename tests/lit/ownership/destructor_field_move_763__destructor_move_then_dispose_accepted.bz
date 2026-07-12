// Ported from tests/ownership/destructor_field_move_763_test.cpp
//   (BEZEL_TEST destructor_field_move_763, destructor_move_then_dispose_accepted).
//
// §28.8.1: `var taken := move this._inner; taken.Dispose();` in a destructor —
// the field need not be restored (the object is being torn down) and the taken
// value is discharged. ACCEPTED (the #763 repro).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Outer private owned _inner: Inner; public constructor() this._inner := new Inner(); end constructor; destructor var taken := move this._inner; taken.Dispose(); end destructor; public static method Main(): int return 0; end Main; end Outer;

// NODIAG-NOT: error
// NODIAG-NOT: §
