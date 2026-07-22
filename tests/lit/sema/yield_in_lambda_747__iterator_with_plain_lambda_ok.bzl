// Ported from tests/sema/yield_in_lambda_747_test.cpp
//   (BEZEL_TEST yield_in_lambda_747, iterator_with_plain_lambda_ok).
//
// §29.9: an iterator method that also contains a yield-FREE lambda is fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Nums(): IEnumerable<int> var f: (int) -> int := lambda(x) => x + 1; yield return f(1); yield break; end Nums; public static method Main(): int return 0; end Main; end F;

// NODIAG-NOT: error
// NODIAG-NOT: §
