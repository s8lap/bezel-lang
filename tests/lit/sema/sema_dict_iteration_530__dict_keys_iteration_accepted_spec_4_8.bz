// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, dict_keys_iteration_accepted_spec_4_8).
//
// §4.8 / §4.9: `for var k in d.Keys` — Keys is ICollection<K>, which is
// iterable. The guard on Dictionary must not affect this pattern.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var k in d.Keys do n := n + 1; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
