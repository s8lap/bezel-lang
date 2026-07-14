// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.196 / #1844: the self-referential-bound rule applies to a
// generic METHOD's `where` clause exactly as to a generic type's — the check
// runs over each method declaration's constraints, not only the enclosing
// type's. Fails-without-fix: before #1844 the method compiled rc=0.

module M;

public class C
  public static method F<T>(x: T): T where T: T => x;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: self-referential constraint on type parameter `T`
// CHECK-SAME: (§14.2)
