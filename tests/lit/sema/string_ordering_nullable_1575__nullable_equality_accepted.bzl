// Ported from tests/sema/string_ordering_nullable_1575_test.cpp
//   (BEZEL_TEST string_ordering_nullable_1575, nullable_equality_accepted).
//
// §8.4 (#1575): `=` on `string?` stays legal and null-safe — this fix must
// not touch equality, only the four ordering operators.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a: string? := null; var b: string? := "x"; if a = b then return 1; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
