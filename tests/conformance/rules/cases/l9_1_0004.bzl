//! case: l9_1_0004
//! doc: stdlib
//! section: 9.1
//! title: int.MinValue and int.MaxValue are the exact compiler-provided constants
//! rule: public static const MinValue: int := -2_147_483_648; public
//!   static const MaxValue: int := 2_147_483_647;
//! cites: 9.1
//! expect: run
//! output: -2147483648
//!   2147483647
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.MinValue);
    Console.WriteLine(int.MaxValue);
  end Main;
end Program;
