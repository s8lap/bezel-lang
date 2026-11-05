//! case: s5_1_0904
//! doc: spec
//! section: 5.1
//! title: a negative int argument at a (decimal) -> decimal function value keeps its sign
//! rule: | `int` | `long`, `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: run
//! output: -7
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var i: int := -7;
    Console.WriteLine(g(i));
  end Main;
end Program;
