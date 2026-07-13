// Ported from tests/sema/move_bare_field_restoration_825_test.cpp
//   (BEZEL_TEST move_bare_field_restoration_825, bare_local_move_unaffected).
//
// §28.8.1: a bare `move` of a LOCAL variable (not a field) must not trigger
// the field-restoration check — exprIsOwnedFieldRead correctly returns false
// for a non-field local.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public constructor() end constructor; destructor end destructor; end R; class C public static method Main(): int var r := new R(); var s := move r; s.Dispose(); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
