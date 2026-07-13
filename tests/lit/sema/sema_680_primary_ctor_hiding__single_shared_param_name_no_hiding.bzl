// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, single_shared_param_name_no_hiding).
//
// §11.1 / #680: a single shared param name in base and derived must not diagnose.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A(x: int) private _x: int := x; end A; class B(x: int, y: int) : A(x) private _y: int := y; end B;

// NODIAG-NOT: error
// NODIAG-NOT: §
