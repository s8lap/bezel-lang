//! case: s4_6_1204
//! doc: spec
//! section: 4.6
//! title:a method group concatenated inside a null-conditional call argument is rejected
//! rule:The compiler does not implicitly convert other types to `string`; use interpolation or
//!   `.ToString()`.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method G(): int => 5;

  public static method Main()
    var s: string? := "abc";
    var t := s?.Replace("a", "x=" + G);
    Console.WriteLine(t ?? "n");
  end Main;
end Program;
