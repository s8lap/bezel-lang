// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, class_implements_iequatable_resolves_2_9).
//
// §2.9 / §2.3: A class implementing IEquatable<Self> with Equals must
// resolve the base-type name without error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module Test; class Point : IEquatable<Point> private _x: int; private _y: int; public constructor(x: int, y: int) _x := x; _y := y; end constructor; public method Equals(other: Point): bool return _x = other._x and _y = other._y; end Equals; public override method GetHashCode(): int return _x; end GetHashCode; end Point;

// NODIAG-NOT: error
// NODIAG-NOT: §
