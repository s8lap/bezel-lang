//! case: l2_2_0035
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a readonly double? field narrowed by a null test
//! rule: Narrowing applies to locals and `readonly` fields only (§7.4, v0.350): "no call, `await` or closure invalidates the fact" on a readonly-field carrier. After `if c.g <> null`, `c.g` is double for Abs(2.5) -> 2.5 and Max(2.5, 1.0) -> 2.5, with a call (`Max`) between the test and the second use.
//! cites: 2.2, 7.4
//! expect: run
//! output: 2.5
//!   2.5
module Conf;

public class Box
  public readonly g: double?;
  public constructor(a: double?) this.g := a; end constructor;
end Box;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var c := new Box(IdD(2.5));
    if c.g <> null then
      Console.WriteLine(Math.Abs(c.g));
      Console.WriteLine(Math.Max(c.g, 1.0));
    end;
  end Main;
end Program;
