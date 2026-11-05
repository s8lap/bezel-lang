//! case: s4_6_1202
//! doc: spec
//! section: 4.6
//! title:a string plus the result of a void built-in call is rejected
//! rule:The compiler does not implicitly convert other types to `string`; use interpolation or
//!   `.ToString()`.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("x=" + Console.WriteLine("a"));
  end Main;
end Program;
