// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, default_value_record_and_array_accepted_951).
//
// ACCEPT: default(record of value fields) and default([N]value-type).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int); class C public static method Main(): int var p: Point := default(Point); var a: [3]int := default([3]int); return p.X + a[0]; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
