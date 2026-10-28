//! case: l9_3_0009
//! doc: stdlib
//! section: 9.3
//! title: decimal.Zero and decimal.One are the constants 0m and 1m
//! rule: stdlib §9.3 (v0.345) "Rendering and range": "`Zero` is `0m` and `One` is `1m`".
//! cites: 9.3
//! expect: run
//! output: 0\n1
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Zero);
    Console.WriteLine(decimal.One);
  end Main;
end Program;
