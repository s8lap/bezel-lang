//! case: s28_3_10_7402
//! doc: spec
//! section: 28.3.10
//! title: an async lambda returning one of two owned locals disposes the other exactly once
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10
//! expect: run
//! output: D1
//!   D2
//!   2
//!   D2
//!   D1
//!   1
//! notes: Pins #6061 (third repro, multi-return OwningTask<Dq>): each Dq is disposed exactly once on
//!   both return paths.
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
    var f := async lambda(b: bool): OwningTask<Dq> => do
      var o := new Dq(1); var q := new Dq(2);
      if b then return q; end if;
      return o;
    end;
    using var d := await f(c);
    return d.id;
  end F;
  public static async method Main(): Task<int>
    Console.WriteLine(await F(true));
    Console.WriteLine(await F(false));
    return 0;
  end Main;
end Program;
