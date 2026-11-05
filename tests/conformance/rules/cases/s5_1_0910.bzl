//! case: s5_1_0910
//! doc: spec
//! section: 5.1
//! title: a double argument at a (decimal) -> decimal function value is rejected
//! rule: Between `decimal` and binary floats (`float`, `double`).
//! cites: 27.7
//! expect: reject
//! diagnostic: never implicit
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (decimal) -> decimal := lambda(x: decimal) => x;
    var i: double := 7.0;
    Console.WriteLine(g(i));
  end Main;
end Program;
