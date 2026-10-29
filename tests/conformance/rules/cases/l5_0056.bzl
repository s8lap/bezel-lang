//! case: l5_0056
//! doc: stdlib
//! section: 5
//! title: A function-typed parameter named Count is called with the IList Count property as its argument
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); Count(l.Count) calls the parameter with the property value 3, and 3 * 2 = 6.
//! cites: 22, 26.5, 4.1
//! expect: run
//! output: 6
module Conf;

public class Z
  public static method F(Count: (int) -> int, l: IList<int>): int => Count(l.Count);
end Z;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(Z.F(lambda(x) => x * 2, a));
  end Main;
end Program;
