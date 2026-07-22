// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_where_with_valid_predicate_accepted_spec_5).
//
// §5 / #542: `arr.Where((x) => x > 0)` — a LINQ Where with a valid predicate
// lambda. Must not be broken by the factory-arg check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var e := arr.Where(lambda(x) => x > 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
