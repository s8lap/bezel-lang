// Ported from tests/ownership/destructor_owned_field_dispose_1230_test.cpp
//   (BEZEL_TEST destructor_owned_field_dispose_1230, move_then_dispose_in_destructor_accepted).
//
// §28.8.1 / §28.5 (#1230 anti-over-reject): `move _field` in a destructor
// takes ownership out and suppresses the auto-cleanup for that field — using
// a local `var taken := move _sub; taken.Dispose();` is the canonical
// early-dispose pattern and must NOT be rejected. ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Inner public n: int := 0; destructor end destructor; end Inner; class Widget private owned _sub: Inner; public constructor() _sub := new Inner(); end constructor; destructor var taken := move _sub; taken.Dispose(); end destructor; public static method Main(): int return 0; end Main; end Widget;

// NODIAG-NOT: error
// NODIAG-NOT: §
