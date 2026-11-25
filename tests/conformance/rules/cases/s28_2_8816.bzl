//! case: s28_2_8816
//! doc: spec
//! section: 28.2
//! title: a lambda holder read before a finally kill of its source is admitted
//! rule: what is rejected is the use the kill makes certain to fault, and a disposal after the borrow's last use changes no verdict
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 2
//! notes: Accepting twin of 8815 (the smallest edit: the read of `y.tag` moves before the kill). r = 1 + 1 = 2.
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
        r := 1 + y.tag;
      finally
        x.Dispose();
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
