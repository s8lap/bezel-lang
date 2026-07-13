// Ported from tests/sema/inline_array_element_count_test.cpp
//   (BEZEL_TEST inline_array_element_count, exact_elements_accepted).
//
// §15.4: exactly N elements is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a: [3]int := [10, 20, 30]; return a[0]; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
