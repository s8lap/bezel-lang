// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, owned_dict_getoradd_int_factory_arg_is_rejected_spec_4_9).
//
// §4.9 / #542: passing an int literal as the factory argument to
// OwningDictionary.GetOrAdd must also be rejected.
// od is OwningDictionary<string, Val>; factory must be (string) -> owned Val.

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) _ := od.GetOrAdd("a", 99); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: GetOrAdd expects a factory function `(K) -> V` as its second argument (stdlib §4.8 / §4.9 / #542); got a non-function value instead.
