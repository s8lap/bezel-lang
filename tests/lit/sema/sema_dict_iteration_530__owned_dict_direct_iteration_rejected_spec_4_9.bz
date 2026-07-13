// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, owned_dict_direct_iteration_rejected_spec_4_9).
//
// §4.9 v0.135: `for var e in od` on an OwningDictionary<string,int> parameter
// must still be REJECTED (the #530 diagnostic is narrowed to OwningDictionary
// only in v0.135; Dictionary is now accepted).

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var e in od do n := n + 1; end; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: OwningDictionary<K,V> is not directly iterable (stdlib §4.9); use `d.Drain()` (ownership-transfer) instead of `for var e in d`
