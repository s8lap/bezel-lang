// Ported from tests/sema/primitive_assignability_test.cpp
//   (BEZEL_TEST sema_assign, int_literal_in_range_sbyte_ok_738).
//
// In-range narrowing (incl. negative within sbyte) stays valid.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M() var y: sbyte := -100; var z: byte := 200; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
