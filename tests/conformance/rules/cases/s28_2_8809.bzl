//! case: s28_2_8809
//! doc: spec
//! section: 28.2
//! title: a lambda for-loop body that kills a borrowed source and then breaks is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 1
//! notes: Pins #9098 fix-forward (6d9000066). The `for` twin of 8807: one read of `y.tag`, the kill, then break; s = 1.
module Conf;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1);
      var y := x;
      var s := 0;
      for var i in [0, 1] do
        s := s + y.tag;
        x.Dispose();
        break;
      end for;
      return s;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
