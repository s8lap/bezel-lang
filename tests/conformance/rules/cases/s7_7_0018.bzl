//! case: s7_7_0018
//! doc: spec
//! section: 7.7
//! title: a uint right operand of ?? into an int? left operand is not implicitly convertible
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§7.7]).
//!   "The following are never implicit:" ([§5.1]) "Between signed and unsigned types of the same or
//!   narrower width" ([§5.1]): `uint` to `int` is that conversion, so the right operand does not
//!   convert to `int`.
//! cites: 7.7, 5.1
//! expect: reject
//! diagnostic: implicit
//! issue: 6675
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    var d: uint := 3000000000;
    Console.WriteLine(a ?? d);
  end Main;
end Program;
