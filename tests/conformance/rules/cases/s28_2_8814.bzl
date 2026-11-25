//! case: s28_2_8814
//! doc: spec
//! section: 28.2
//! title: a lambda holder re-bound to the variable's new value after a finally kill is admitted
//! rule: takes a fresh borrow
//! cites: 28.2, 28.3.10
//! expect: run
//! stdout: 3
//! notes: Pins #9098 (305cc38a9, the finally kill). x is killed in the finally, re-assigned `new Res(2)`, and `y := x`
//!  takes a fresh borrow of the new value; r = 1 + 2 = 3. The method form agrees.
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
        x.Dispose();
      end try;
      x := new Res(2);
      y := x;
      r := r + y.tag;
      x.Dispose();
      return r;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
