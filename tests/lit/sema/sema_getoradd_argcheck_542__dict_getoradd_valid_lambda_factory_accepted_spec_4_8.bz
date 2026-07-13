// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, dict_getoradd_valid_lambda_factory_accepted_spec_4_8).
//
// §4.8 / #542: a valid lambda factory `(k) => 7` — the factory is a function
// and the call is well-formed. Must produce no errors.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var v: int := d.GetOrAdd("a", (k) => 7); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
