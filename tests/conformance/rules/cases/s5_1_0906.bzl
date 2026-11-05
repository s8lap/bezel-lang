//! case: s5_1_0906
//! doc: spec
//! section: 5.1
//! title: a (decimal) -> decimal method reference called with uint and byte arguments widens them to
//!   decimal
//! rule: | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//! cites: 27.6, 27.7
//! expect: run
//! output: 4294967295
//!   200
//! issue: 8158
module Conf;

public class Program
  public static method Id(x: decimal): decimal
    return x;
  end Id;

  public static method Main()
    var g: (decimal) -> decimal := Program.Id;
    var i: uint := 4294967295u;
    var b: byte := 200;
    Console.WriteLine(g(i));
    Console.WriteLine(g(b));
  end Main;
end Program;
