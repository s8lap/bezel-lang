// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_signature_test.cpp
//   (BEZEL_TEST sema_overload, owned_only_difference_diagnoses).

module M; class T end T; class C public method F(t: T) _ := 1; end F; public method F(owned t: T) _ := 2; end F; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: overload of `F` is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers; neither `owned` (ordinary or on an `out` parameter) nor `scoped` distinguishes
// CHECK-SAME: (§22.6/§11.3, v0.181)
