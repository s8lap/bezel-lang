// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, auto_property_ok).
//
// §23.3: a plain auto-property has an implicit setter — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public property Val: int; public static method Main(): int using var c := new C(); c.Val := 7; return c.Val; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
