//! case: l9_3_0010
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse admits leading zeros and renders the exact value
//! rule: stdlib §9.3 (v0.345): "`007.50` [is] admitted" — the parsed value is the exact
//!   decimal value, which renders without trailing zeros.
//! cites: 9.3
//! expect: run
//! stdout: 7.5
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Parse("007.50"));
  end Main;
end Program;
