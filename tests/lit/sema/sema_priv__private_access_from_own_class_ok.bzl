// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, private_access_from_own_class_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C private _x: int := 1; public method M() _ := this._x; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
