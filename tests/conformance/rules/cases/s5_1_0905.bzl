//! case: s5_1_0905
//! doc: spec
//! section: 5.1
//! title: a negative sbyte argument at a (decimal) -> decimal function value keeps its sign
//! rule: | `sbyte` | `short`, `int`, `long`, `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: run
//! output: -5
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var s: sbyte := -5;
    Console.WriteLine(g(s));
  end Main;
end Program;
