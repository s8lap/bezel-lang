// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, dict_getoradd_bool_factory_arg_is_rejected_spec_4_8).
//
// §4.8 / #542: passing a bool literal as the factory argument must produce
// a sema error.

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var v: int := d.GetOrAdd("a", true); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: GetOrAdd expects a factory function `(K) -> V` as its second argument (stdlib §4.8 / §4.9 / #542); got a non-function value instead.
