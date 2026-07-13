// Ported from tests/sema/const_assignment_747_test.cpp
//   (BEZEL_TEST const_assignment_747, constant_expressions_accepted).
//
// §22.12 regression guard: literals, operations over constants, references to
// other consts, and conditionals over constants are all admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const A: int := 40; public const B: int := A * 2 + 1; public const S: string := "hi"; public const F: bool := true; public const G: int := A if F else B; public static method Main(): int return A + B; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
