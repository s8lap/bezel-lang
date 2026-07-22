// Ported from tests/sema/with_visibility_934_test.cpp
//   (BEZEL_TEST with_visibility_934, private_property_with_from_declaring_type_accepted).
//
// §13.1, #934 — ACCEPT: `with` targets a private member from INSIDE the
// declaring type. Private IS visible to the declaring type (§13.1).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record MyRec private property Secret: int; public constructor(s: int) Secret := s; end constructor; public method WithSecret(x: int): MyRec return this with(Secret := x); end WithSecret; end MyRec; class C public static method Main(): int var r := new MyRec(42); var r2 := r.WithSecret(99); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
