// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example5_no_reachable_candidate).
//
// §22.6 Example 5 — no reachable candidate.
// float -> int is not implicit (float's only implicit target is double);
// float -> decimal is not implicit. No candidate is applicable → error.

module M; class Transformer public method Transform(x: int): int => x; public method Transform(x: decimal): int => 0; public method Use(n: float) _ := this.Transform(n); end Use; end Transformer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no overload of `Transform` is applicable to these arguments; every candidate would require a conversion that §5.1 does not admit as implicit
// CHECK-SAME: (§22.6)
