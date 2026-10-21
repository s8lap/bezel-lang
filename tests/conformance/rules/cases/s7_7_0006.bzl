//! case: s7_7_0006
//! doc: spec
//! section: 7.7
//! title: a long constant that fits the int left type of ?? converts to it by value
//! rule: "for a left operand of type `T?`, the right operand must be implicitly convertible" ([§5.1])
//!   "to `T` or to `T?`; the result type is `T` when the right operand is non-nullable and `T?` when it is nullable".
//!   "An expression of integer type that is a **compile-time constant**" "implicitly converts to any integer type whose range contains its value" ([§5.1]), and "the source may be a compile-time constant of **any** integer type — not only `int`".
//! cites: 7.7, 5.1
//! expect: run
//! output: 5
//!   2
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var a: int? := null;
    var b: int? := 2;
    Console.WriteLine(a ?? 5L);
    Console.WriteLine(b ?? 5L);
  end Main;
end Program;
