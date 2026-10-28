//! case: l9_3_0012
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse admits a leading minus sign and a fractional part
//! rule: stdlib §9.3 (v0.345): "`Parse` accepts an optional leading sign (`+` or `-`) … `"-0.25"`
//!   [is] admitted".
//! cites: 9.3
//! expect: run
//! stdout: -0.25
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Parse("-0.25"));
  end Main;
end Program;
