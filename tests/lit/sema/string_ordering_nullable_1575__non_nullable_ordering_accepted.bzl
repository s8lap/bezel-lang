// Ported from tests/sema/string_ordering_nullable_1575_test.cpp
//   (BEZEL_TEST string_ordering_nullable_1575, non_nullable_ordering_accepted).
//
// §4.6 base case: non-nullable `string < string` keeps compiling — the fix
// only gates nullable operands.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a: string := "a"; var b: string := "b"; if a < b then return 1; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
