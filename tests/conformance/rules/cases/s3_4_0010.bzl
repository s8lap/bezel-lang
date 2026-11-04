//! case: s3_4_0010
//! doc: spec
//! section: 3.4
//! title: String.IsNullOrWhiteSpace and string.IsNullOrWhiteSpace agree
//! rule: public static pure method IsNullOrWhiteSpace(value: string?): bool;
//! cites: 3.4, 9.6
//! expect: run
//! output: True
//!   True
//! issue: 7494
//! notes: twin of s3_4_0011 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.IsNullOrWhiteSpace(" "));
    Console.WriteLine(string.IsNullOrWhiteSpace(" "));
  end Main;
end Program;
