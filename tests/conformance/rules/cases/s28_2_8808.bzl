//! case: s28_2_8808
//! doc: spec
//! section: 28.2
//! title: a lambda loop body that kills inside try/finally and then breaks is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 2
//! notes: Pins #9098 fix-forward (6d9000066). The try kills x and the finally runs `s + 1`; the trailing break leaves
//!  the loop, so no path reaches the next iteration's `y.tag`. s = 1 (read) + 1 (finally) = 2.
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
      while true do
        s := s + y.tag;
        try
          x.Dispose();
        finally
          s := s + 1;
        end try;
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
