//! case: s7_5_4503
//! doc: spec
//! section: 7.5
//! title: named arguments of a method call right of a null-conditional bind on the
//!   path where the chain runs
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path
//!   where it runs
//! cites: 22.5
//! expect: run
//! output: sub: el
//!   sub2: el
//!   plain: el
//! issue: 6070
module Conf;

public class Box
  public Name: string := "Hello";
end Box;

public class Program
  public static method Main()
    var a: Box? := new Box();
    var s: string? := a?.Name.Substring(startIndex := 1, length := 2);
    Console.WriteLine($"sub: {s}");
    var t: string? := a?.Name.Substring(length := 2, startIndex := 1);
    Console.WriteLine($"sub2: {t}");
    var p: string := a!.Name.Substring(length := 2, startIndex := 1);
    Console.WriteLine($"plain: {p}");
  end Main;
end Program;
