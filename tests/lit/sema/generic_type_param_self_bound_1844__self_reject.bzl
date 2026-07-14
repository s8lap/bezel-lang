// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 / #1844: "A self-referential bound (`where T: T`) ... is a
// compile error at the declaration." A type-parameter bound whose head names the
// bounded parameter itself is rejected at the declaration, independent of any
// instantiation. Fails-without-fix: before #1844 the declaration compiled rc=0
// (no declaration-time check existed), so `%not` would see a zero exit.

module M;

public class Box<T> where T: T
  public V: T?;
end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: self-referential constraint on type parameter `T`
// CHECK-SAME: (§14.2)
