//! case: l5_0058
//! doc: stdlib
//! section: 5
//! title: The IList Count property is accepted as a nested call argument
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); l.Count is a property read, not a Count call, so it is an ordinary int argument.
//! cites: 4.1
//! expect: run
//! output: 4
//!   6
module Conf;

public class N
  public static method G(a: int, b: int): int => a + b;
  public static method F(x: int): int => x;
end N;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var l: IList<int> := a;
    Console.WriteLine(N.G(1, N.F(l.Count)));
    Console.WriteLine(N.G(N.F(l.Count), N.F(l.Count)));
  end Main;
end Program;
