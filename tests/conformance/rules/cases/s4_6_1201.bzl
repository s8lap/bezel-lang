//! case: s4_6_1201
//! doc: spec
//! section: 4.6
//! title:a string plus a static method group is rejected
//! rule:The compiler does not implicitly convert other types to `string`; use interpolation or
//!   `.ToString()`.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method G(): int => 5;

  public static method Main()
    Console.WriteLine("x=" + G);
  end Main;
end Program;
