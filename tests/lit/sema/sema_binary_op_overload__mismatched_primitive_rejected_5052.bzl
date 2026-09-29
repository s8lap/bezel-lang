// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #5052 (§25, v0.307): `operator *(v: V, n: int)` was found applicable for
// `v * "x"` because the primitive-parameter check only compared TypeRef::Kind,
// never the underlying PrimitiveType enum — `string` and `int` both have
// Kind::Primitive so they matched. §5.1 has no implicit conversion from string
// to int, so the overload should NOT be applicable.

module M;
record V(X: int)
  public static method operator *(v: V, n: int): V => new V(v.X * n);
end V;
public class Program
  public static method Main(): int
    var v := new V(1);
    var w := v * "x";   // string is not convertible to int
    return 0;
  end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `*` is not defined for type `V`
// CHECK-SAME: with these operands — declare a matching `operator *` whose parameters
// CHECK-SAME: match the operand order (Bezel does not auto-commute operator overloads)
// CHECK-SAME: (§25)
