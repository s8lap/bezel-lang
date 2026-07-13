// Ported from tests/sema/readonly_field_test.cpp
//   (BEZEL_TEST sema_readonly, non_readonly_assigned_in_method_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: int := 0; public method M() this._x := 1; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
