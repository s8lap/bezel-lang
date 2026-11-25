//! case: s28_2_8810
//! doc: spec
//! section: 28.2
//! title: a lambda loop body that discards the source binding and then breaks is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 1
//! notes: Pins #9098 fix-forward (6d9000066). The discard `_ := x;` is the kill; the trailing break leaves the loop, so
//!  the next iteration's `x.tag` is never reached. s = 1.
module Conf;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1);
      var s := 0;
      while true do
        s := s + x.tag;
        _ := x;
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
