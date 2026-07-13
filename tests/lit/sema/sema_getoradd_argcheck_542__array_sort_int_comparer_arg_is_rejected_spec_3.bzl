// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_sort_int_comparer_arg_is_rejected_spec_3).
//
// §3 / #542: `arr.Sort(99)` — Sort(comparer) with an int literal must be
// rejected by sema. Without the check, codegen treats 99 as a closure pointer
// → malformed IR / crash.

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) arr.Sort(99); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Sort expects a comparer function `(T, T) -> int` as its argument (stdlib §3 / #542); got a non-function value instead.
