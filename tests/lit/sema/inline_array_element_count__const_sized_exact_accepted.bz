// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, const_sized_exact_accepted).
//
// §15.1: a `const`-folded size N also enforces the exact-N rule.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const N: int := 3; public static method Main(): int var a: [N]int := [7, 8, 9]; return a[0] + a[1] + a[2]; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
