//! case: s7_7_4803
//! doc: spec
//! section: 7.7
//! title: a nullable left operand with a widening or reference-convertible right operand is admitted
//! rule: for a left operand of type `T?`, the right operand must be implicitly convertible ([§5.1](#sec-implicit-conversions)) to `T` or to `T?`
//! cites: 7.7, 5.1
//! expect: run
//! output: 5
//!   3
//!   s
module Conf;

public class Program
  public static method Main()
    var a: long? := null;
    var b := a ?? 5;
    var c: int? := 3;
    var d: long := c ?? 7;
    var e: Object? := null;
    var f := e ?? "s";
    Console.WriteLine(b);
    Console.WriteLine(d);
    Console.WriteLine(f);
  end Main;
end Program;
