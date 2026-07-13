// Ported from tests/sema/icomparable_iequatable_576_test.cpp
//   (BEZEL_TEST sema_576_icomparable_iequatable, record_implements_icomparable_resolves_2_8_9_2).
//
// §2.8 / §9.2: Records may implement IComparable<T> (it is not IDisposable).
// Spec §9.6 example: record Point(X: int, Y: int): IComparable<Point>.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int): IComparable<Point> public method CompareTo(other: Point): int return X - other.X; end CompareTo; end Point;

// NODIAG-NOT: error
// NODIAG-NOT: §
