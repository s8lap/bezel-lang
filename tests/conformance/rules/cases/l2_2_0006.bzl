//! case: l2_2_0006
//! doc: stdlib
//! section: 2.2
//! title: the decimal overload of Abs returns a decimal
//! rule: The declaration "public static pure method Abs(x: decimal): decimal;" returns a decimal, and a
//!   decimal times `0m` is the decimal zero, which renders `0`.
//! expect: run
//! stdout: 0
//! issue: 6297
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Abs(-2.5m) * 0m);
  end Main;
end Program;
