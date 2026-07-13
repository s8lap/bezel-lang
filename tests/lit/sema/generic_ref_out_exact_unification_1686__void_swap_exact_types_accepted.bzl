// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, void_swap_exact_types_accepted).
//
// §14.5 / #1686 control: the SAME void `Swap<T>` called with two `ref int`
// arguments — both `ref` occurrences agree exactly. Must remain accepted
// (no over-rejection of the exact ref/out case).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method Swap<T>(ref a: T, ref b: T) var temp := a; a := b; b := temp; end Swap; end Utils; class C public static method Main(): int var x: int := 1; var y: int := 2; Utils.Swap(ref x, ref y); return x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
