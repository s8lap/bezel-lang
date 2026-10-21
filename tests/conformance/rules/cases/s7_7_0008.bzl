//! case: s7_7_0008
//! doc: spec
//! section: 7.7
//! title: a double constant is not implicitly convertible to the int left type of ??
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`". "A value of one type implicitly converts to another when no programmer-visible information is lost" ([§5.1]), and a `double` to `int` conversion loses information, so it is not implicit.
//! cites: 7.7, 5.1
//! expect: reject
//! diagnostic: implicitly convert
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    Console.WriteLine(a ?? 3.5);
  end Main;
end Program;
