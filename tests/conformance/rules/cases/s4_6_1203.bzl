//! case: s4_6_1203
//! doc: spec
//! section: 4.6
//! title:a void built-in call plus a string in a var initializer is rejected
//! rule:The compiler does not implicitly convert other types to `string`; use interpolation or
//!   `.ToString()`.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    var s := Console.Write("a") + "b";
    Console.WriteLine(s);
  end Main;
end Program;
