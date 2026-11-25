//! case: s28_2_8817
//! doc: spec
//! section: 28.2
//! title: a lambda holder read after a returning try's finally kill is rejected
//! rule: a use of the holder — a read or member access through it, passing it, re-borrowing it, invoking the closure, enumerating or disposing the frame — is the same compile error
//! cites: 28.2, 28.3.10
//! expect: reject
//! diagnostic: disposed
//! notes: Pins #9098 (305cc38a9). The only path to the read of `y.tag` runs through the catch and the finally, which
//!  disposes x on every path; the holder use is the dominating-kill error.
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
        return 1;
      catch e: Exception =>
        r := 2;
      finally
        x.Dispose();
      end try;
      return r + y.tag;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
