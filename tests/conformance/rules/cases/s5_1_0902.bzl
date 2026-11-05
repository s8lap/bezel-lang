//! case: s5_1_0902
//! doc: spec
//! section: 5.1
//! title: a uint argument at a (decimal) -> decimal function value widens to decimal and keeps its value
//! rule: | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: run
//! output: 4294967295
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var i: uint := 4294967295u;
    Console.WriteLine(g(i));
  end Main;
end Program;
