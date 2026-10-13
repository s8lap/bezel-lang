// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/overload_signature_test.cpp
//   (BEZEL_TEST sema_overload, duplicate_constructors_diagnose).

module M; class C public constructor(a: int) end constructor; public constructor(a: int) end constructor; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: overload of constructor is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers; `owned` and `scoped` never distinguish overloads
// CHECK-SAME: (§22.6/§11.3, v0.338)
