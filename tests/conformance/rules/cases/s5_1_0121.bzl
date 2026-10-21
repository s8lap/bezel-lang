//! case: s5_1_0121
//! doc: spec
//! section: 5.1
//! title: a decimal value does not implicitly convert to int
//! rule: "A value of one type implicitly converts to another when no programmer-visible information is
//!   lost" ([§5.1]). The `decimal` result of `Math.Abs(-2.5m)` has a fractional part that an `int`
//!   cannot hold, so the conversion is not implicit.
//! cites: 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    var n: int := Math.Abs(-2.5m);
    Console.WriteLine(n);
  end Main;
end Program;
