// Ported from tests/sema/yield_in_lambda_747_test.cpp
//   (BEZEL_TEST yield_in_lambda_747, yield_in_iterator_method_ok).
//
// §29.9: `yield` in a genuine ITERATOR METHOD is fine — no §29.9 error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Nums(): IEnumerable<int> yield return 1; yield break; end Nums; public static method Main(): int return 0; end Main; end F;

// NODIAG-NOT: error
// NODIAG-NOT: §
