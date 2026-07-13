// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, with_on_new_record_accepted_9_5).
//
// ACCEPT: `with` on a FRESHLY-`new`-constructed RECORD is still ordinary
// record `with` — §9.5's own example: "new Point(1, 2) with Y := 9 end has
// a record-typed source and is an ordinary record `with`". The
// discriminator is the source's type kind (record), not the absence of a
// prior binding.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int); class C public static method Main(): int var p := new Point(1, 2) with Y := 9 end; return p.X + p.Y; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
