// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #5053 (§7.11, v0.303): a `string?` argument binding to a primitive
// `string` operator parameter. The Primitive branch in `operandMatchesParam`
// (ExprTypecheck_Exprs.cpp:5551) only checked kind, not nullability — so a
// `string?` matched a bare `string` parameter and the operator resolved,
// producing malformed IR at codegen.
//
// After the fix: the nullable-primitive / non-nullable-primitive mismatch
// causes `operandMatchesParam` to return false, the operator does not
// resolve, and the generic "operator not defined" diagnostic fires.

module M;
record V(X: int)
  public static method operator *(v: V, s: string): V => new V(v.X + s.Length);
end V;
public class Program
  public static method Main(): int
    var v := new V(1);
    var sn: string? := "ab";
    var w := v * sn; // CHECK: operator `*` is not defined for type `V` with these operands
    return 0;
  end Main;
end Program;
