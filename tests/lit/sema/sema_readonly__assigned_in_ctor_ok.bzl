// Ported from tests/sema/readonly_field_test.cpp
//   (BEZEL_TEST sema_readonly, assigned_in_ctor_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _x: int; public constructor() this._x := 1; end constructor; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
