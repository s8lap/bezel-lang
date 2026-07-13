// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_sorted_int_comparer_arg_is_rejected_spec_3).
//
// §3 / #472 / #542: `arr.Sorted(99)` — Sorted(comparer) with an int literal
// must be rejected by sema.  Without the check, codegen treats 99 as a
// closure pointer → malformed IR / crash.

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var s := arr.Sorted(99); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: Sorted expects a comparer function `(T, T) -> int` as its argument (stdlib §3 / #472 / #542); got a non-function value instead.
