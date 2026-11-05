//! case: s5_1_0907
//! doc: spec
//! section: 5.1
//! title: a int argument at a (ulong) -> ulong function value is rejected
//! rule: | `int` | `long`, `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: reject
//! diagnostic: implicit
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (ulong) -> ulong := lambda(x: ulong) => x;
    var i: int := 7;
    Console.WriteLine(g(i));
  end Main;
end Program;
