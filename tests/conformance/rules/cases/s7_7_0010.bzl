//! case: s7_7_0010
//! doc: spec
//! section: 7.7
//! title: a long? right operand of ?? into an int? left operand is not implicitly convertible
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`". "A value of one type implicitly converts to another when no programmer-visible information is lost" ([§5.1]), and a `long` value does not implicitly convert to an `int`.
//! cites: 7.7, 5.1
//! expect: reject
//! diagnostic: implicitly convert
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    var b: long? := 4;
    var r := a ?? b;
    Console.WriteLine(r ?? 0);
  end Main;
end Program;
