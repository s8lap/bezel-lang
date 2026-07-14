// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 / #1844: "... a cycle among type-parameter bounds
// (`where T: U` together with `where U: T`) is a compile error at the
// declaration." The 2-cycle `T: U`, `U: T` is rejected at the declaration.
// Fails-without-fix: before #1844 the declaration compiled rc=0.

module M;

public class Box2<T, U> where T: U where U: T
  public V: T?;
end Box2;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cyclic constraint among type parameters
// CHECK-SAME: (§14.2)
