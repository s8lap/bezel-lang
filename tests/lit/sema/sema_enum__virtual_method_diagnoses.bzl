// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, virtual_method_diagnoses).

module M; enum E A, B; public virtual method M() _ := 1; end M; end E;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `M` may not be virtual, abstract, or override — enums dispatch statically
// CHECK-SAME: (§10.5)
