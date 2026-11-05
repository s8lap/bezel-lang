//! case: s5_1_0901
//! doc: spec
//! section: 5.1
//! title: a byte argument at a (decimal) -> decimal function value widens to decimal and keeps its value
//! rule: | `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: run
//! output: 200
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var i: byte := 200;
    Console.WriteLine(g(i));
  end Main;
end Program;
