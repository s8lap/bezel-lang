// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.5, §28.3 K3.14, v0.338): the OTHER polarity of
// owning_match_move_scrutinee_consumes_3575_silent.bzl. The unmarked
// `case a of …` is a BORROWING match ("the unmarked read is a borrow … an
// unmarked read never silently consumes"), so `a` is still Owned at scope end
// and the §28.3 must-use warning on its declaration must STILL fire. A fix
// that discharged `a` for any `case` over it would silence this row.

// CHECK: warning: owned value in `a` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)
// CHECK-NOT: error

module M;
class Resource
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method N(): int => _n;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class C
  public static method Main(): int
    var a := Bundle.Ready(new Resource(1));
    case a of
      Ready(var r) => Console.WriteLine("r"),
      Empty => Console.WriteLine("e")
    end;
    return 0;
  end Main;
end C;
