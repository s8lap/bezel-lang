//! case: l9_3_0011
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse admits an optional leading plus sign
//! rule: stdlib §9.3 (v0.345): "`Parse` accepts an optional leading sign (`+` or `-`) …
//!   `"+5"` [is] admitted".
//! cites: 9.3
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Parse("+5"));
  end Main;
end Program;
