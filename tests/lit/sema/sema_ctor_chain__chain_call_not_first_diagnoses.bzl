// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/ctor_chain_test.cpp
//   (BEZEL_TEST sema_ctor_chain, chain_call_not_first_diagnoses).

module M; class B public constructor() end constructor; end B; class D : B public _x: int := 0; public constructor() this._x := 1; base(); end constructor; end D;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `base(...)` / `this(...)` constructor chain call may appear only as the first statement
// CHECK-SAME: (§12.8)
