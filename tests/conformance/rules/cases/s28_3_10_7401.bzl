//! case: s28_3_10_7401
//! doc: spec
//! section: 28.3.10
//! title: a plain lambda block body inside an async method disposes its owned local at its end
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: D1
//!   3
//! notes: Pins #6061 (second repro: plain lambda inside an async method).
module Conf;
public class Dq
  public id: int;
  public constructor(i: int) this.id := i; end constructor;
  destructor
    Console.WriteLine("D" + this.id.ToString());
  end destructor;
end Dq;
public class Program
  public static async method F(c: bool): Task<int>
    var f := lambda(b: bool): int => do
      var o := new Dq(1);
      return 3;
    end;
    return f(c);
  end F;
  public static async method Main(): Task<int>
    var k := await F(true);
    Console.WriteLine(k);
    return 0;
  end Main;
end Program;
