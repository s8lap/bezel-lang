// Ported from tests/sema/primary_ctor_param_init_478_test.cpp
//   (BEZEL_TEST sema_primary_ctor_param_init_478, property_init_from_primary_ctor_param_ok).
//
// §22.10 spec example shape: `public readonly property X: int := x;` where
// `x` is a primary-constructor parameter. Must compile without error (#478).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Point(x: int, y: int) public readonly property X: int := x; public readonly property Y: int := y; end Point;

// NODIAG-NOT: error
// NODIAG-NOT: §
