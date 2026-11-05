//! case: s5_1_0909
//! doc: spec
//! section: 5.1
//! title: a decimal argument at a (double) -> double function value is rejected
//! rule: Between `decimal` and binary floats (`float`, `double`).
//! cites: 27.7
//! expect: reject
//! diagnostic: never implicit
//! issue: 8158
module Conf;

public class Program
  public static method Main()
    var g: (double) -> double := lambda(x: double) => x;
    var i: decimal := 7.0m;
    Console.WriteLine(g(i));
  end Main;
end Program;
