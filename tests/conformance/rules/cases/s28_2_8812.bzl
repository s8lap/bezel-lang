//! case: s28_2_8812
//! doc: spec
//! section: 28.2
//! title: a lambda loop body whose kill and break sit in a do block is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 1
//! notes: Pins #9098 fix-forward (6d9000066). The kill and the break are the last statement of the loop body, wrapped
//!  in a `do` block; the block never falls through, so no path reaches the next iteration. s = 1.
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
        do
          x.Dispose();
          break;
        end;
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
