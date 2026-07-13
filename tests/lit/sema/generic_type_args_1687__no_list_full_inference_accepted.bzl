// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, no_list_full_inference_accepted).
//
// Positive control: NO explicit type-argument list — full argument-driven
// inference (§14.5 phase 1) must still work and stay diagnostic-free.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method Pick<TFirst, TSecond>(a: TFirst, b: TSecond): TFirst return a; end Pick; end Utils; class C public static method Main(): int var x := Utils.Pick(1, "s"); return x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
