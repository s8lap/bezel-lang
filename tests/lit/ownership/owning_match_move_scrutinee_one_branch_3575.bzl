// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK %s
//
// fix-forward #3575 (§35.5, §28.3 rule 4 / K7.4, v0.338): `case move a` on only
// ONE branch of an `if`. The join is maybe-consumed: §28.3 rule 4 says
// "scope-end disposal is per-path", so the not-taken path still implicitly
// drops `a` there. This row pins the behaviour of the sibling spelling
// `var b := move a;` in the same position (`other` below) -- both must
// behave identically, which is what "the same K3.4 consume" requires.
//
// CHECK-DAG: warning: owned value in `a` is implicitly disposed
// CHECK-DAG: warning: owned value in `other` is implicitly disposed
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
    var other := Bundle.Ready(new Resource(3));
    var n := 3;
    if n > 2 then
      case move a of
        Ready(var r) => Console.WriteLine("r"),
        Empty => Console.WriteLine("e")
      end;
      var b := move other;
    end;
    return 0;
  end Main;
end C;
