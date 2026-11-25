//! case: s28_2_8807
//! doc: spec
//! section: 28.2
//! title: a lambda loop body that kills a borrowed source and then breaks is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 1
//! notes: Pins #9098 fix-forward (6d9000066). `y := x` borrows x; the loop body reads `y.tag`, kills x, and breaks, so no
//!  path reaches a later use of y. One iteration runs: s = 1.
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
      var i := 0;
      var s := 0;
      while i < 2 do
        s := s + y.tag;
        x.Dispose();
        break;
      end while;
      return s;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
