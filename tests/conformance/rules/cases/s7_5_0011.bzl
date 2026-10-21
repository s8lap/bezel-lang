//! case: s7_5_0011
//! doc: spec
//! section: 7.5
//! title: the result of a null-conditional Count call is nullable and does not convert to int
//! rule: "The result type is always nullable" ([§7.5]), so a non-nullable `int` binding of `a?.Count()`
//!   is a compile error.
//! cites: 7.5
//! expect: reject
//! diagnostic: nullable
//! issue: 6529
module Conf;

public class Program
  public static method Main()
    var a: Array<int>? := [4, 2];
    var n: int := a?.Count();
    Console.WriteLine(n);
  end Main;
end Program;
