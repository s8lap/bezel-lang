//! case: l2_5_0047
//! doc: stdlib
//! section: 2.5
//! title: a DateTime constructor argument produced by an await raises on an invalid month in an async method
//! rule: ArgumentOutOfRangeException on invalid components
//! cites: 2.5, 30.1
//! expect: run
//! output: 100
module Conf;

public class Tag
  public N: string;
  public constructor(n: string) N := n; end constructor;
end Tag;

public class Program
  public static async method F(): Task<int>
    return 13;
  end F;
  public static async method A(i: int): Task<int>
    using var r := new Tag("x");
    var d := new DateTime(2020, await F(), 1 + i);
    return d.Month + r.N.Length;
  end A;
  public static async method Main(): Task<int>
    var caught := 0;
    for var i in 0..<100 do
      try
        var x := await Program.A(i);
      catch ArgumentException => caught := caught + 1; end;
    end;
    Console.WriteLine(caught);
    return 0;
  end Main;
end Program;
