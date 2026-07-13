// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/ctor_chain_test.cpp
//   (BEZEL_TEST sema_ctor_chain, two_chain_calls_diagnoses).

module M; class B public constructor() end constructor; end B; class D : B public constructor() base(); base(); end constructor; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `base(...)` / `this(...)` constructor chain call may appear only as the first statement
// CHECK-SAME: (§12.8)
