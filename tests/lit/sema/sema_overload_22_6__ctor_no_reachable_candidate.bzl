// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, ctor_no_reachable_candidate).
//
// §22.6 / §22.7 — no reachable constructor: a float argument is not
// implicitly convertible to int or to decimal. `new T(n)` is an error.

module M; class T public constructor(a: int) end constructor; public constructor(a: decimal) end constructor; end T; class C public method M(n: float) _ := new T(n); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no overload of `new T` is applicable to these arguments; every candidate would require a conversion that §5.1 does not admit as implicit
// CHECK-SAME: (§22.6)
