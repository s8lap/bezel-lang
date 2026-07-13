// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, set_iteration_unaffected_spec_4_10).
//
// §4.10 / §29.2: `for var x in s` over a Set<int> parameter.
// Set is iterable (implements IEnumerable<T>); the guard must not reject it.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var x in s do n := n + x; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
