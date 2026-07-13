// Ported from tests/sema/private_visibility_test.cpp
//   (BEZEL_TEST sema_priv, public_access_from_other_class_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public _x: int := 1; end A; class B public method M(a: A) _ := a._x; end M; end B;

// NODIAG-NOT: error
// NODIAG-NOT: §
