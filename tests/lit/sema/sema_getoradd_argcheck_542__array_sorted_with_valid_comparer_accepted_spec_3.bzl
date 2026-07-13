// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_sorted_with_valid_comparer_accepted_spec_3).
//
// §3 / #472 / #542: `arr.Sorted((a, b) => a - b)` with a valid comparer.
// Must not be broken by the factory-arg check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var s := arr.Sorted((a, b) => a - b); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
