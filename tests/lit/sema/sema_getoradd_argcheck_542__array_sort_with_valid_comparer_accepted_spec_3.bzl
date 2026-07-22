// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_sort_with_valid_comparer_accepted_spec_3).
//
// §3 / #542: `arr.Sort((a, b) => a - b)` with a valid comparer.
// Must compile clean — the Sort(comparer) check must not reject valid lambdas.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) arr.Sort(lambda(a, b) => a - b); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
