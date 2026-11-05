//! case: s5_1_0903
//! doc: spec
//! section: 5.1
//! title: a ulong argument at a (decimal) -> decimal function value widens to decimal and keeps its value
//! rule: | `ulong` | `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: run
//! output: 18446744073709551615
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var i: ulong := 18446744073709551615ul;
    Console.WriteLine(g(i));
  end Main;
end Program;
