//! case: s5_1_0908
//! doc: spec
//! section: 5.1
//! title: a long argument at a (int) -> int function value is rejected
//! rule: | `long` | `float`, `double`, `decimal` |
//! cites: 27.7
//! expect: reject
//! diagnostic: implicit
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (int) -> int := lambda(x: int) => x;
    var i: long := 7;
    Console.WriteLine(g(i));
  end Main;
end Program;
