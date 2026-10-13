// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.5 form 2, §28.3 K3.4 / K3.14, v0.338): the marked
// owning-match scrutinee `case move a of …` CONSUMES `a` ("the consumed
// binding is left moved-from when the construct completes"), so there is no
// scope-end implicit drop and therefore no §28.3 must-use warning on `a`.
// `var b := move a;` (K3.4's sibling spelling) is already silent.
//
// ABSENCE row: it needs a positive anchor or it is vacuous. `d` is a
// construction that is never discharged (the §35.4 must-use positive), and the
// warning for `d` MUST appear; the CHECK-NOT lines bracket it on both sides
// so the position of the `a` diagnostic does not matter. The program
// compiles: a warning is not a rejection.

// CHECK-NOT: owned value in `a`
// CHECK: warning: owned value in `d` is implicitly disposed at the end of its scope
// CHECK-NOT: owned value in `a`
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
    var d := Bundle.Ready(new Resource(2));
    case move a of
      Ready(var r) => Console.WriteLine("r"),
      Empty => Console.WriteLine("e")
    end;
    Console.WriteLine("body");
    return 0;
  end Main;
end C;
