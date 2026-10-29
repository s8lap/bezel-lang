// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.8.1 / Appendix A.7 MoveExpr alternative (c) (v0.350): "At (c) a `with`
// clause is a compile error" - the operand is a bare identifier naming a local
// binding. Conformance case s28_8_1_0023, now rejected by the sema rule rather
// than by a syntax error.
//
// CHECK-NOT: retired pre-v0.208
// CHECK-NOT: expected ';' after expression statement
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `with` clause is not admitted on the `move` of the local binding or parameter `a`

module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Program
  public method Demo()
    var a := new Res();
    using var t := move a with new Res();
  end Demo;
end Program;
