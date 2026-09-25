// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_signature_test.cpp
//   (BEZEL_TEST sema_overload, out_vs_out_owned_clashes).

module M; class T end T; class C public method F(out c: T) _ := 1; end F; public method F(out owned c: T) _ := 2; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: overload of `F` is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers, or by ordinary `owned` under the declaration guard; `out owned` and `scoped` never distinguish. At a position whose type fails the guard the pair collapses to one signature
// CHECK-SAME: (§22.6/§11.3, v0.250)
