// Ported from tests/sema/compound_assign_operator_result_851_test.cpp
//   (BEZEL_TEST compound_assign_operator_result_851, plus_returns_same_type_accepted).
//
// Accepted: an `operator +` that returns the LHS type — the desugared
// `p := p + q` is well-typed.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int) public static method operator +(a: Point, b: Point): Point => new Point(a.X + b.X, a.Y + b.Y); end Point; class C public static method Main(): int var p := new Point(3, 4); var q := new Point(5, 6); p += q; return p.X; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
