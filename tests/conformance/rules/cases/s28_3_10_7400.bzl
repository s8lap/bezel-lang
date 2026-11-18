//! case: s28_3_10_7400
//! doc: spec
//! section: 28.3.10
//! title: an async lambda block body disposes its undischarged owned local at its end
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: D1
//!   3
//! notes: Pins #6061 (owned local in a lambda block body that is never discharged): the destructor
//!   runs exactly once and before the await resumes.
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
    var f := async lambda(b: bool): Task<int> => do
      var o := new Dq(1);
      return 3;
    end;
    return await f(c);
  end F;
  public static async method Main(): Task<int>
    var k := await F(true);
    Console.WriteLine(k);
    return 0;
  end Main;
end Program;
