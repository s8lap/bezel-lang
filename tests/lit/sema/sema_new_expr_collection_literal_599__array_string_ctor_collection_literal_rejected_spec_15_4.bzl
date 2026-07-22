// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_expr_collection_literal_599_test.cpp
//   (BEZEL_TEST sema_new_expr_collection_literal_599, array_string_ctor_collection_literal_rejected_spec_15_4).
//
// §15.4 / §22.7 (#599 must-fail): `new Array<string>(["a", "b"])` — same
// issue with a string element type.

module M; class C public static method M() new Array<string>(["a", "b"]); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal `[..]` cannot be passed as an argument to `new Array<...>` — the constructor takes an optional `int` capacity, not a collection literal; use a typed-target declaration instead: `var x: Array<T> := [...]`
// CHECK-SAME: (§15.4, #599)
