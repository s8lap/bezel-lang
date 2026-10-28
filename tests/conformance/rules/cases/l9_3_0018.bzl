//! case: l9_3_0018
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rounds an over-long in-range value
//! rule: stdlib §9.3 (v0.345): "An in-range value with more digits than a `decimal` holds is
//!   rounded … `"0.12345678901234567890123456785"` parses to `0.1234567890123456789012345678`".
//! cites: 9.3
//! expect: run
//! stdout: 0.1234567890123456789012345678
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Parse("0.12345678901234567890123456785"));
  end Main;
end Program;
