// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, array_iteration_unaffected_spec_4_3).
//
// §4.3 / §29.2: `for var x in arr` over an Array<int> parameter.
// The generic-arg guard must leave Array<T> completely unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var x in arr do n := n + x; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
