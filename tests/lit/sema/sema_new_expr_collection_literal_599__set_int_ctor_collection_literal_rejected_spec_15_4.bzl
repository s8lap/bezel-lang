// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/new_expr_collection_literal_599_test.cpp
//   (BEZEL_TEST sema_new_expr_collection_literal_599, set_int_ctor_collection_literal_rejected_spec_15_4).
//
// §15.4 (#599 must-fail): a Set<int> ctor also takes an optional int
// capacity, not a collection literal.

module M; class C public static method M() new Set<int>([1, 2, 3]); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal `[..]` cannot be passed as an argument to `new Set<...>` — the constructor takes an optional `int` capacity (and/or an `IEqualityComparer<K>`), not a collection literal; bind the literal to a named typed binding first (`var xs: Set<T> := [...]`) and transfer its elements through `Add`/`AddLast`/etc. (§15.4, #2725)
