// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.8.1 *The replacing `move`* / Appendix A.7 `MoveExpr ::= "move"
// PrimaryExpr ("with" Expression)?` (v0.350), issue #7567: `move loc with e` at
// an owned field PARSES (it used to die in the §9.5 record-update suffix with
// the retired-spelling note). The exchange is not lowered yet (issue #5923), so
// sema declines it precisely instead.
//
// CHECK-NOT: retired pre-v0.208
// CHECK-NOT: expected ';' after expression statement
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the replacing `move` (`move loc with e`, §28.8.1

module M;

public class Res
  public constructor()
  end constructor;
end Res;

public class Holder
  public owned F: Res := new Res();
  public constructor()
  end constructor;
end Holder;

public class User
  public static method Swap(h: Holder): owned Res
    return move h.F with new Res();
  end Swap;
end User;
