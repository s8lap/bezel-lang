//! case: s28_2_8813
//! doc: spec
//! section: 28.2
//! title: a lambda finally block holding a loop that kills and breaks is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 2
//! notes: Pins #9098 fix-forward (6d9000066). The try sets r = 1; the finally loop reads `y.tag` (r = 2), kills x and
//!  breaks; nothing reads y afterwards. Result 2.
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
      var r := 0;
      try
        r := 1;
      finally
        while true do
          r := r + y.tag;
          x.Dispose();
          break;
        end while;
      end try;
      return r;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
