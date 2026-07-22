// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §20 / A.7 IfExpr, v0.208 (impl #2079) — the atom's shape rules:
//   1. `else` is mandatory — an expression produces a value on every path.
//   2. `then` separates each condition from its branch.
module M;
class C
  public static method A(c: bool): int
    return (if c then 1);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: expected 'else'
    // CHECK-SAME: mandatory
    // CHECK-SAME: (§20, v0.208)
  end A;

  public static method B(c: bool): int
    return (if c 1 else 2);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: expected 'then' after the `(if {{.*}})` atom's condition (§20, v0.208)
  end B;
end C;
