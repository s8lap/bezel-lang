// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/base_abstract_call_874_test.cpp
//   (BEZEL_TEST sema_base_abstract_call_874, abstract_base_method_call_rejected).
//
// §12.3/§12.5, #874: `base.Area()` where `Area` is abstract must be rejected.

module M; abstract class Shape public abstract method Area(): int; end Shape; class Rectangle : Shape public override method Area(): int var ba: int := base.Area(); return ba; end Area; end Rectangle;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot call abstract base method `Area` via `base` — it has no implementation
// CHECK-SAME: (§12.3/§12.5)
