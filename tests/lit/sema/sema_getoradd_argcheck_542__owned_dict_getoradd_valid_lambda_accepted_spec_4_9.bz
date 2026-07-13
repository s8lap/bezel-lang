// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, owned_dict_getoradd_valid_lambda_accepted_spec_4_9).
//
// §4.9 / #542: a valid lambda factory on OwningDictionary.GetOrAdd must be
// accepted without errors.
// od is OwningDictionary<string, Val>; factory returns owned Val.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var v: Val := od.GetOrAdd("a", (k) => new Val()); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
